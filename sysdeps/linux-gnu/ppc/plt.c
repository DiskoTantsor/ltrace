#include <gelf.h>
#include <sys/ptrace.h>
#include <errno.h>
#include <error.h>
#include <inttypes.h>
#include <assert.h>
#include <string.h>

#include "proc.h"
#include "common.h"
#include "library.h"
#include "breakpoint.h"
#include "linux-gnu/trace.h"

/* There are two PLT types on 32-bit PPC: old-style, BSS PLT, and
 * new-style "secure" PLT.  We can tell one from the other by the
 * flags on the .plt section.  If it's +X (executable), it's BSS PLT,
 * otherwise it's secure.
 *
 * BSS PLT works the same way as most architectures: the .plt section
 * contains trampolines and we put breakpoints to those.  With secure
 * PLT, the .plt section doesn't contain instructions but addresses.
 * The real PLT table is stored in .text.  Addresses of those PLT
 * entries can be computed, and it fact that's what the glink deal
 * below does.
 *
 * If not prelinked, BSS PLT entries in the .plt section contain
 * zeroes that are overwritten by the dynamic linker during start-up.
 * For that reason, ltrace realizes those breakpoints only after
 * .start is hit.
 *
 * 64-bit PPC is more involved.  Program linker creates for each
 * library call a _stub_ symbol named xxxxxxxx.plt_call.<callee>
 * (where xxxxxxxx is a hexadecimal number).  That stub does the call
 * dispatch: it loads an address of a function to call from the
 * section .plt, and branches.  PLT entries themselves are essentially
 * a curried call to the resolver.  When the symbol is resolved, the
 * resolver updates the value stored in .plt, and the next time
 * around, the stub calls the library function directly.  So we make
 * at most one trip (none if the binary is prelinked) through each PLT
 * entry, and correspondingly that is useless as a breakpoint site.
 *
 * Note the three confusing terms: stubs (that play the role of PLT
 * entries), PLT entries, .plt section.
 *
 * We first check symbol tables and see if we happen to have stub
 * symbols available.  If yes we just put breakpoints to those, and
 * treat them as usual breakpoints.  The only tricky part is realizing
 * that there can be more than one breakpoint per symbol.
 *
 * The case that we don't have the stub symbols available is harder.
 * The following scheme uses two kinds of PLT breakpoints: unresolved
 * and resolved (to some address).  When the process starts (or when
 * we attach), we distribute unresolved PLT breakpoints to the PLT
 * entries (not stubs).  Then we look in .plt, and for each entry
 * whose value is different than the corresponding PLT entry address,
 * we assume it was already resolved, and convert the breakpoint to
 * resolved.  We also rewrite the resolved value in .plt back to the
 * PLT address.
 *
 * When a PLT entry hits a resolved breakpoint (which happens because
 * we put back the unresolved addresses to .plt), we move the
 * instruction pointer to the corresponding address and continue the
 * process as if nothing happened.
 *
 * When unresolved PLT entry is called for the first time, we need to
 * catch the new value that the resolver will write to a .plt slot.
 * We also need to prevent another thread from racing through and
 * taking the branch without ltrace noticing.  So when unresolved PLT
 * entry hits, we have to stop all threads.  We then single-step
 * through the resolver, until the .plt slot changes.  When it does,
 * we treat it the same way as above: convert the PLT breakpoint to
 * resolved, and rewrite the .plt value back to PLT address.  We then
 * start all threads again.
 *
 * In theory we might find the exact instruction that will update the
 * .plt slot, and emulate it, updating the PLT breakpoint immediately,
 * and then just skip it.  But that's even messier than the thread
 * stopping business and single stepping that needs to be done.
 *
 * Short of doing this we really have to stop everyone.  There is no
 * way around that.  Unless we know where the stubs are, we don't have
 * a way to catch a thread that would use the window of opportunity
 * between updating .plt and notifying ltrace about the singlestep.
 */

#define PPC_PLT_STUB_SIZE 16
#define PPC64_PLT_STUB_SIZE 8 //xxx

static inline int
host_powerpc64()
{
#ifdef __powerpc64__
	return 1;
#else
	return 0;
#endif
}

GElf_Addr
arch_plt_sym_val(struct ltelf *lte, size_t ndx, GElf_Rela *rela)
{
	if (lte->ehdr.e_machine == EM_PPC && lte->arch.secure_plt) {
		assert(lte->arch.plt_stub_vma != 0);
		return lte->arch.plt_stub_vma + PPC_PLT_STUB_SIZE * ndx;

	} else if (lte->ehdr.e_machine == EM_PPC) {
		return rela->r_offset;

	} else {
		/* If we get here, we don't have stub symbols.  In
		 * that case we put brakpoints to PLT entries the same
		 * as the PPC32 secure PLT case does.  */
		assert(lte->arch.plt_stub_vma != 0);
		return lte->arch.plt_stub_vma + PPC64_PLT_STUB_SIZE * ndx;
	}
}

int
arch_translate_address(struct Process *proc,
		       target_address_t addr, target_address_t *ret)
{
	if (proc->e_machine == EM_PPC64) {
		assert(host_powerpc64());
		long l = ptrace(PTRACE_PEEKTEXT, proc->pid, addr, 0);
		if (l == -1 && errno) {
			error(0, errno, ".opd translation of %p", addr);
			return -1;
		}
		*ret = (target_address_t)l;
		return 0;
	}

	*ret = addr;
	return 0;
}

void *
sym2addr(struct Process *proc, struct library_symbol *sym)
{
	return sym->enter_addr;
}

static GElf_Addr
get_glink_vma(struct ltelf *lte, GElf_Addr ppcgot, Elf_Data *plt_data)
{
	Elf_Scn *ppcgot_sec = NULL;
	GElf_Shdr ppcgot_shdr;
	if (ppcgot != 0
	    && elf_get_section_covering(lte, ppcgot,
					&ppcgot_sec, &ppcgot_shdr) < 0)
		error(0, 0, "DT_PPC_GOT=%#"PRIx64", but no such section found",
		      ppcgot);

	if (ppcgot_sec != NULL) {
		Elf_Data *data = elf_loaddata(ppcgot_sec, &ppcgot_shdr);
		if (data == NULL || data->d_size < 8 ) {
			error(0, 0, "couldn't read GOT data");
		} else {
			// where PPCGOT begins in .got
			size_t offset = ppcgot - ppcgot_shdr.sh_addr;
			assert(offset % 4 == 0);
			uint32_t glink_vma;
			if (elf_read_u32(data, offset + 4, &glink_vma) < 0) {
				error(0, 0, "couldn't read glink VMA address"
				      " at %zd@GOT", offset);
				return 0;
			}
			if (glink_vma != 0) {
				debug(1, "PPC GOT glink_vma address: %#" PRIx32,
				      glink_vma);
				return (GElf_Addr)glink_vma;
			}
		}
	}

	if (plt_data != NULL) {
		uint32_t glink_vma;
		if (elf_read_u32(plt_data, 0, &glink_vma) < 0) {
			error(0, 0, "couldn't read glink VMA address");
			return 0;
		}
		debug(1, ".plt glink_vma address: %#" PRIx32, glink_vma);
		return (GElf_Addr)glink_vma;
	}

	return 0;
}

static int
load_dynamic_entry(struct ltelf *lte, int tag, GElf_Addr *valuep)
{
	Elf_Scn *scn;
	GElf_Shdr shdr;
	if (elf_get_section_type(lte, SHT_DYNAMIC, &scn, &shdr) < 0
	    || scn == NULL) {
	fail:
		error(0, 0, "Couldn't get SHT_DYNAMIC: %s",
		      elf_errmsg(-1));
		return -1;
	}

	Elf_Data *data = elf_loaddata(scn, &shdr);
	if (data == NULL)
		goto fail;

	size_t j;
	for (j = 0; j < shdr.sh_size / shdr.sh_entsize; ++j) {
		GElf_Dyn dyn;
		if (gelf_getdyn(data, j, &dyn) == NULL)
			goto fail;

		if(dyn.d_tag == tag) {
			*valuep = dyn.d_un.d_ptr;
			return 0;
		}
	}

	return -1;
}

static int
load_ppcgot(struct ltelf *lte, GElf_Addr *ppcgotp)
{
	return load_dynamic_entry(lte, DT_PPC_GOT, ppcgotp);
}

static int
load_ppc64_glink(struct ltelf *lte, GElf_Addr *glinkp)
{
	return load_dynamic_entry(lte, DT_PPC64_GLINK, glinkp);
}

int
arch_elf_init(struct ltelf *lte)
{
	lte->arch.secure_plt = !(lte->plt_flags & SHF_EXECINSTR);
	if (lte->ehdr.e_machine == EM_PPC && lte->arch.secure_plt) {
		GElf_Addr ppcgot;
		if (load_ppcgot(lte, &ppcgot) < 0) {
			error(0, 0, "couldn't find DT_PPC_GOT");
			return -1;
		}
		GElf_Addr glink_vma = get_glink_vma(lte, ppcgot, lte->plt_data);

		assert (lte->relplt_size % 12 == 0);
		size_t count = lte->relplt_size / 12; // size of RELA entry
		lte->arch.plt_stub_vma = glink_vma
			- (GElf_Addr)count * PPC_PLT_STUB_SIZE;
		debug(1, "stub_vma is %#" PRIx64, lte->arch.plt_stub_vma);

	} else if (lte->ehdr.e_machine == EM_PPC64) {
		GElf_Addr glink_vma;
		if (load_ppc64_glink(lte, &glink_vma) < 0) {
			error(0, 0, "couldn't find DT_PPC64_GLINK");
			return -1;
		}

		/* The first glink stub starts at offset 32.  */
		lte->arch.plt_stub_vma = glink_vma + 32;
	}

	/* On PPC64, look for stub symbols in symbol table.  These are
	 * called: xxxxxxxx.plt_call.callee_name@version+addend.  */
	if (lte->ehdr.e_machine == EM_PPC64
	    && lte->symtab != NULL && lte->strtab != NULL) {

		/* N.B. We can't simply skip the symbols that we fail
		 * to read or malloc.  There may be more than one stub
		 * per symbol name, and if we failed in one but
		 * succeeded in another, the PLT enabling code would
		 * have no way to tell that something is missing.  We
		 * could work around that, of course, but it doesn't
		 * seem worth the trouble.  So if anything fails, we
		 * just pretend that we don't have stub symbols at
		 * all, as if the binary is stripped.  */

		size_t i;
		for (i = 0; i < lte->symtab_count; ++i) {
			GElf_Sym sym;
			if (gelf_getsym(lte->symtab, i, &sym) == NULL) {
				struct library_symbol *sym, *next;
			fail:
				for (sym = lte->arch.stubs; sym != NULL; ) {
					next = sym->next;
					library_symbol_destroy(sym);
					free(sym);
					sym = next;
				}
				lte->arch.stubs = NULL;
				break;
			}

			const char *name = lte->strtab + sym.st_name;

#define STUBN ".plt_call."
			if ((name = strstr(name, STUBN)) == NULL)
				continue;
			name += sizeof(STUBN) - 1;
#undef STUBN

			size_t len;
			const char *ver = strchr(name, '@');
			if (ver != NULL) {
				len = ver - name;

			} else {
				/* If there is "+" at all, check that
				 * the symbol name ends in "+0".  */
				const char *add = strrchr(name, '+');
				if (add != NULL) {
					assert(strcmp(add, "+0") == 0);
					len = add - name;
				} else {
					len = strlen(name);
				}
			}

			char *sym_name = strndup(name, len);
			struct library_symbol *libsym = malloc(sizeof(*libsym));
			if (sym_name == NULL || libsym == NULL) {
				free(sym_name);
				free(libsym);
				goto fail;
			}

			target_address_t addr
				= (target_address_t)sym.st_value + lte->bias;
			library_symbol_init(libsym, addr, sym_name, 1,
					    LS_TOPLT_EXEC);
			libsym->arch.type = PPC64PLT_STUB;
			libsym->next = lte->arch.stubs;
			lte->arch.stubs = libsym;
		}
	}

	return 0;
}

static int
read_plt_slot_value(struct Process *proc, GElf_Addr addr, GElf_Addr *valp)
{
	/* on PPC32 we need to do things differently, but PPC64/PPC32
	 * is currently not supported anyway.  */
	assert(host_powerpc64());

	long l = ptrace(PTRACE_PEEKTEXT, proc->pid, addr, 0);
	if (l == -1 && errno != 0) {
		error(0, errno, "ptrace .plt slot value @%#" PRIx64, addr);
		return -1;
	}

	*valp = (GElf_Addr)l;
	return 0;
}

static int
unresolve_plt_slot(struct Process *proc, GElf_Addr addr, GElf_Addr value)
{
	/* We only modify plt_entry[0], which holds the resolved
	 * address of the routine.  We keep the TOC and environment
	 * pointers intact.  Hence the only adjustment that we need to
	 * do is to IP.  */
	if (ptrace(PTRACE_POKETEXT, proc->pid, addr, value) < 0) {
		error(0, errno, "unresolve .plt slot");
		return -1;
	}
	return 0;
}

enum plt_status
arch_elf_add_plt_entry(struct Process *proc, struct ltelf *lte,
		       const char *a_name, GElf_Rela *rela, size_t ndx,
		       struct library_symbol **ret)
{
	if (lte->ehdr.e_machine == EM_PPC)
		return plt_default;

	/* PPC64.  If we have stubs, we return a chain of breakpoint
	 * sites, one for each stub that corresponds to this PLT
	 * entry.  */
	struct library_symbol *chain = NULL;
	struct library_symbol **symp;
	for (symp = &lte->arch.stubs; *symp != NULL; ) {
		struct library_symbol *sym = *symp;
		if (strcmp(sym->name, a_name) != 0) {
			symp = &(*symp)->next;
			continue;
		}

		/* Re-chain the symbol from stubs to CHAIN.  */
		*symp = sym->next;
		sym->next = chain;
		chain = sym;
	}

	if (chain != NULL) {
		*ret = chain;
		return plt_ok;
	}

	/* We don't have stub symbols.  Find corresponding .plt slot,
	 * and check whether it contains the corresponding PLT address
	 * (or 0 if the dynamic linker hasn't run yet).  N.B. we don't
	 * want read this from ELF file, but from process image.  That
	 * makes a difference if we are attaching to a running
	 * process.  */

	GElf_Addr plt_entry_addr = arch_plt_sym_val(lte, ndx, rela);
	GElf_Addr plt_slot_addr = rela->r_offset;
	assert(plt_slot_addr >= lte->plt_addr
	       || plt_slot_addr < lte->plt_addr + lte->plt_size);

	GElf_Addr plt_slot_value;
	if (read_plt_slot_value(proc, plt_slot_addr, &plt_slot_value) < 0)
		return plt_fail;

	char *name = strdup(a_name);
	struct library_symbol *libsym = malloc(sizeof(*libsym));
	if (name == NULL || libsym == NULL) {
		error(0, errno, "allocation for .plt slot");
	fail:
		free(name);
		free(libsym);
		return plt_fail;
	}

	library_symbol_init(libsym, (target_address_t)plt_entry_addr,
			    name, 1, LS_TOPLT_EXEC);
	libsym->arch.plt_slot_addr = plt_slot_addr;

	if (plt_slot_value == plt_entry_addr || plt_slot_value == 0) {
		libsym->arch.type = PPC64PLT_UNRESOLVED;
		libsym->arch.resolved_value = plt_entry_addr;

	} else {
		/* Unresolve the .plt slot.  If the binary was
		 * prelinked, this makes the code invalid, because in
		 * case of prelinked binary, the dynamic linker
		 * doesn't update .plt[0] and .plt[1] with addresses
		 * of the resover.  But we don't care, we will never
		 * need to enter the resolver.  That just means that
		 * we have to un-un-resolve this back before we
		 * detach, which is nothing new: we already need to
		 * retract breakpoints.  */

		if (unresolve_plt_slot(proc, plt_slot_addr, plt_entry_addr) < 0)
			goto fail;
		libsym->arch.type = PPC64PLT_RESOLVED;
		libsym->arch.resolved_value = plt_slot_value;
	}

	*ret = libsym;
	return plt_ok;
}

void
arch_elf_destroy(struct ltelf *lte)
{
	struct library_symbol *sym;
	for (sym = lte->arch.stubs; sym != NULL; ) {
		struct library_symbol *next = sym->next;
		library_symbol_destroy(sym);
		free(sym);
		sym = next;
	}
}

static enum callback_status
keep_stepping_p(struct process_stopping_handler *self)
{
	struct Process *proc = self->task_enabling_breakpoint;
	struct library_symbol *libsym = self->breakpoint_being_enabled->libsym;
	GElf_Addr value;
	if (read_plt_slot_value(proc, libsym->arch.plt_slot_addr, &value) < 0)
		return CBS_FAIL;

	/* In UNRESOLVED state, the RESOLVED_VALUE in fact contains
	 * the PLT entry value.  */
	if (value == libsym->arch.resolved_value)
		return CBS_CONT;

	/* The .plt slot got resolved!  We can migrate the breakpoint
	 * to RESOLVED and stop single-stepping.  */
	if (unresolve_plt_slot(proc, libsym->arch.plt_slot_addr,
			       libsym->arch.resolved_value) < 0)
		return CBS_FAIL;
	libsym->arch.type = PPC64PLT_RESOLVED;
	libsym->arch.resolved_value = value;

	return CBS_STOP;
}

static void
ppc64_plt_bp_continue(struct breakpoint *bp, struct Process *proc)
{
	switch (bp->libsym->arch.type) {
		target_address_t rv;
	case PPC64PLT_UNRESOLVED:
		if (process_install_stopping_handler(proc, bp, NULL,
						     &keep_stepping_p,
						     NULL) < 0) {
			perror("ppc64_unresolved_bp_continue: couldn't install"
			       " event handler");
			continue_after_breakpoint(proc, bp);
		}
		return;

	case PPC64PLT_RESOLVED:
		rv = (target_address_t)bp->libsym->arch.resolved_value;
		set_instruction_pointer(proc, rv);
		continue_process(proc->pid);
		return;

	case PPC64PLT_STUB:
		break;
	}

	assert(bp->libsym->arch.type != bp->libsym->arch.type);
	abort();
}

/* For some symbol types, we need to set up custom callbacks.  XXX we
 * don't need PROC here, we can store the data in BP if it is of
 * interest to us.  */
int
arch_breakpoint_init(struct Process *proc, struct breakpoint *bp)
{
	if (proc->e_machine == EM_PPC
	    || bp->libsym == NULL)
		return 0;

	/* We could see LS_TOPLT_EXEC or LS_TOPLT_NONE (the latter
	 * when we trace entry points), but not LS_TOPLT_POINT
	 * anywhere on PPC.  */
	if (bp->libsym->plt_type != LS_TOPLT_EXEC
	    || bp->libsym->arch.type == PPC64PLT_STUB)
		return 0;

	static struct bp_callbacks cbs = {
		.on_continue = ppc64_plt_bp_continue,
	};
	breakpoint_set_callbacks(bp, &cbs);
	return 0;
}

void
arch_breakpoint_destroy(struct breakpoint *bp)
{
}

int
arch_breakpoint_clone(struct breakpoint *retp, struct breakpoint *sbp)
{
	retp->arch = sbp->arch;
	return 0;
}

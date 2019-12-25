#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/wait.h>
#include <lib.h>
#include <syscall.h>
#include <current.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <copyinout.h>
#include <synch.h>
#include <limits.h>
#include <mips/trapframe.h>
#include "opt-A2.h"
#include <vfs.h>
#include <kern/fcntl.h>
#if OPT_A2
// sys_fork implementation takes current trap frame and returns PID of child
// child's return value is handled in enter_forked_process
int sys_fork(struct trapframe *currenttf, pid_t *retval) {
	// create child process and set it's parent as current process
	struct proc *child = proc_create_runprogram(curproc->p_name);
	struct pidTableEntry *childEntry = returnEntry(child->pid);
	childEntry->parentPid = curproc->pid;
	if (child == NULL) {
		DEBUG(DB_SYSCALL, "sys_fork error: unable to create process.\n");
		return ENPROC;
	}
	// copy address space
	as_copy(curproc_getas(), &(child->p_addrspace));
	if (child->p_addrspace == NULL) {
		// copy failed
		DEBUG(DB_SYSCALL, "sys_fork error: Unable to create child address space.\n");
		proc_destroy(child);
		return ENOMEM;
	}
	// create trapframe for child process
	struct trapframe *childtf = kmalloc(sizeof(struct trapframe));
	if (childtf == NULL) {
		DEBUG(DB_SYSCALL, "sys_fork error: Unable to create child trapframe.\n");
		proc_destroy(child);
		return ENOMEM;
	}
	// Copy trapframe
	memcpy(childtf, currenttf, sizeof(struct trapframe));
	// create new thread
	// enter_forked_process takes in childtf and 1 as parameters and modifies child's register values, 
	// handles child's return value, and returns to userspace
	int error = thread_fork(curthread->t_name, child, &enter_forked_process, childtf, 1);
	if (error) {
		proc_destroy(child);
		kfree(childtf);
		childtf = NULL;
		return error;
	}
	// Add child to parent's children array
	struct pidTableEntry *p = returnEntry(curproc->pid);
	lock_acquire(pidTableLock);
	array_add(p->children, (void *)child->pid, NULL);
	lock_release(pidTableLock);
	// Parent process returns child's pid
	*retval = child->pid;
	return 0;
}
#endif /* OPT_A2 */
  /* this implementation of sys__exit does not do anything with the exit code */
  /* this needs to be fixed to get exit() and waitpid() working properly */
// TODO: MEMORY MANAGE ONCE PID IS DEAD
void sys__exit(int exitcode) {
#if OPT_A2
	lock_acquire(pidTableLock);
	struct pidTableEntry *exitProc = returnEntry(curproc->pid);
	
	if (exitProc->parentPid != NO_PARENT) {
		// if the process trying to exit has a parent,
		// it has information that parent might need hence zombie
		exitProc->state = ZOMBIE;
		// store exit status for future use
		exitProc->exitStatus = _MKWAIT_EXIT(exitcode);
		// broadcast other threads waiting
		cv_broadcast(waitTableCV, pidTableLock);
	} else {
		// Doesn't have a parent so we can kill the process and reuse its PID
		exitProc->state = DEAD;
		array_add(reusePIDList, (void *) exitProc->pid, NULL);
		removeFromPidTable(exitProc->pid);
	}
	// Assign the parent PID of the exited process' children as NO_PARENT
	// if child was ZOMBIE, we can now kill it and add it's PID to the
	// reuse list
	unsigned int len = array_num(exitProc->children);
	for (unsigned int i =0; i < len; ++i) {
		pid_t childPid = (pid_t) array_get(exitProc->children, i);
		struct pidTableEntry *child = returnEntry(childPid);
		child->parentPid = NO_PARENT;
		if (child->state == ZOMBIE) {
			child->state = DEAD;
			array_add(reusePIDList, (void *) child->pid, NULL);
			removeFromPidTable(child->pid);
		}
	}
	lock_release(pidTableLock);
#endif /* OPT_A2 */
  struct addrspace *as;
  struct proc *p = curproc;
  /* for now, just include this to keep the compiler from complaining about
     an unused variable */
  (void)exitcode;
  DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);
  KASSERT(curproc->p_addrspace != NULL);
  as_deactivate();
  /*
   * clear p_addrspace before calling as_destroy. Otherwise if
   * as_destroy sleeps (which is quite possible) when we
   * come back we'll be calling as_activate on a
   * half-destroyed address space. This tends to be
   * messily fatal.
   */
  as = curproc_setas(NULL);
  as_destroy(as);
  /* detach this thread from its process */
  /* note: curproc cannot be used after this call */
  proc_remthread(curthread);
  /* if this is the last user process in the system, proc_destroy()
     will wake up the kernel menu thread */
  proc_destroy(p);
  
  thread_exit();
  /* thread_exit() does not return, so we should never get here */
  panic("return from thread_exit in sys_exit\n");
}
/* stub handler for getpid() system call                */
int
sys_getpid(pid_t *retval)
{
#if OPT_A2
	*retval = curproc->pid;
#else 
  /* for now, this is just a stub that always returns a PID of 1 */
  /* you need to fix this to make it work properly */
  *retval = 1;
#endif /*OPT_A2 */
  return(0);
}
/* stub handler for waitpid() system call                */
int
sys_waitpid(pid_t pid,
	    userptr_t status,
	    int options,
	    pid_t *retval)
{
  int exitstatus;
  int result;
#if OPT_A2
	result = 0;
	lock_acquire(pidTableLock);
	struct pidTableEntry *waitForMe = returnEntry(pid);
	if (waitForMe == NULL) {
		// Return error code for process does not exist
		result = ESRCH;
	} else if(!isChild(curproc->pid, pid)) {
		// return error code for non-parent calling waitpid
		result = ECHILD;
	} else if (status == NULL) {
		// status is an invalid pointer
		result = EFAULT;
	} else if (options != 0) {
		result = EINVAL;
	}
	if (result) {
		lock_release(pidTableLock);
		return(result);
	}
#else
  /* this is just a stub implementation that always reports an
     exit status of 0, regardless of the actual exit status of
     the specified process.   
     In fact, this will return 0 even if the specified process
     is still running, and even if it never existed in the first place.
     Fix this!
  */
  if (options != 0) {
	  return(EINVAL);
  }
#endif /* OPT_A2 */
#if OPT_A2
	while (waitForMe->state == ALIVE) {
		// child process has not exited yet, so we need to wait
		cv_wait(waitTableCV, pidTableLock);
	}
	// set exit status once process exits
	exitstatus = waitForMe->exitStatus;
	lock_release(pidTableLock);
#else
  /* for now, just pretend the exitstatus is 0 */
  exitstatus = 0;
#endif
  // move int bytes of exit status from kernel to userspace at address status
  result = copyout((void *)&exitstatus,status,sizeof(int));
  if (result) {
    return(result);
  }
  // Return pid if successful
  *retval = pid;
  return(0);
}
#if OPT_A2
int sys_execv(userptr_t progname, userptr_t args) {
	struct addrspace *oldas, *newas;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result;
	if (progname == NULL) {
		return ENOENT;
	}
	// Count and copy arguments into kernel
	int argc = 0;
      	int i = 0;
	userptr_t kernarg;
	while (kernarg != 0) {
		result = copyin(args + i, &kernarg, sizeof(userptr_t));
		if (result) {
			return result;
		}
		if (kernarg == 0) {
			break;
		}
		if (strlen((char*)kernarg) > 1024) {
			return E2BIG;
		}
		++argc;
		i += 4;
	}
	
	if (argc > 64) {
		return E2BIG;
	}
//	kprintf("arg count: %d\n", argc);
	char** newargs = kmalloc(sizeof(char) * argc);
	if (newargs == NULL) {
		return result;
	}
	
	int off = 0;
	char* kernarg2;
	for (int j = 0; j < argc; ++j) {
		result = copyin(args + off, &kernarg2, sizeof(userptr_t));
		if (result) {
			return result;
		}
		newargs[j] = kmalloc(sizeof(char) * (strlen(kernarg2) + 1));
		result = copyinstr((userptr_t)kernarg2, newargs[j], strlen(kernarg2) + 1, NULL);
		if (result) {
			return result;
		}
		off += 4;
	}
/*	newargs[argc] = NULL;
	for (int c = 0; c < argc; c++) {
		kprintf("newarg %d value: %s\n", c, newargs[c]);
	}*/
	/*
	char **newargs = kmalloc(sizeof(char) * 64);
	if (newargs == NULL) {
		return ENOMEM;
	}
	char* kernarg;
	result = copyin(args, kernarg, 4);
	if (result) {
		return result;
	}
	while (kernarg != 0) {
		if (strlen(kernarg) > 1024) {
			return E2BIG;
		} else {
			newargs[argc] = kmalloc(sizeof(char) * (strlen(kernarg) +1));
			result = copyinstr((userptr_t)kernarg, newargs[argc], strlen(kernarg) + 1, NULL);
			if (result) {
				return result;
			}
			++argc;
			result = copyin(args+4, kernarg, 4);
			if (result) {
				return result;
			}
		}
	}
	newargs[argc] = NULL;
	if (argc > 64) {
		return E2BIG;
	}
	*/
	
	// Copy program path into kernel
	size_t proglen = strlen((char*)progname) + 1;
	char *newprog = kmalloc(sizeof(char) * proglen);
	if (newprog == NULL) {
		return ENOMEM;
	}
	result = copyinstr(progname, newprog, proglen, NULL);
	if (result) {
		return result;
	}
	// Open file
	char *filename_temp;
	filename_temp =  kstrdup((char*) progname);
	result = vfs_open(filename_temp, O_RDONLY, 0, &v);
	kfree(filename_temp);
	if (result) {
		return result;
	}
	// Create new address space, set prcoess to it, and activate
	newas = as_create();
	if (newas == NULL) {
		vfs_close(v);
		return ENOMEM;
	}
	oldas = curproc_setas(newas);
	as_activate();
	// Load ELF
	load_elf(v, &entrypoint);
	if (result) {
		// p_addrspace goes away when curproc destroyed
		vfs_close(v);
		curproc_setas(oldas);
		return result;
	}
	vfs_close(v);
	// Define user stack in newas
	result = as_define_stack(newas, &stackptr);
	if (result) {
		// p_addrspace goes away when curproc destroyed
		curproc_setas(oldas);
		return result;
	}
	
	// create virtual address vargptr, copy in strings, and set up vargptr
	vaddr_t vargptr[argc + 1];
	for (int i = argc - 1; i >= 0; i--) {
		stackptr -= ROUNDUP(strlen(newargs[i]) + 1, 8);
		result = copyoutstr(newargs[i], (userptr_t)stackptr, strlen(newargs[i]) + 1, NULL);
		if (result) {
			return result;
		}
		vargptr[i] = stackptr;
	}
	vargptr[argc] = 0;
	
	for (int i = argc; i >= 0; i--) {
		stackptr -= ROUNDUP(sizeof(vaddr_t), 4);
		result = copyout(&vargptr[i], (userptr_t)stackptr, sizeof(vaddr_t));
		if (result) {
			return result;
		}
	}
	// free newargs on stack;
	int length = sizeof(newargs)/sizeof(char*);
	for (int i = 0; i < length; ++i) {
		kfree(newargs[i]);
	}
	kfree(newargs);
	
	// Destroy old adress space
	as_destroy(oldas);
	
	// Enter new process
	// params: argc, userspace address of argv, stackptr, entrypoint
	enter_new_process(argc, (userptr_t) stackptr, stackptr, entrypoint);
	// enter_new_process should not return
	panic("enter_new_process returned\n");
	return EINVAL;
}
#endif /* OPT_A2 */
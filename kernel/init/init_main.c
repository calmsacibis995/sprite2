/*
 * File:
 * sprite2/kernel/init/init_main.c
 *
 * The main program for Sprite: initializes modules and creates system processes.
 * Also creates a process to run the Init program.
 *
 * Copyright (c) 1986 The Regents of the University of California.
 * All rights reserved.
 */

/*
 * TODO - Updated 2025/04/07:
 *
 * 1. Sprite is currently a monolithic kernel. We want a microkernel, or at least a hybrid,
 * if it isn't feasible.
 *
 * 2. Verify portability. Sprite ran on now obsolete hardware (DECstation 5000 and SPARCstation),
 * which are either not fully emulated or becoming increasingly rare and mostly in the hands of collectors.
 * Initial efforts are to build x86_64, ARM/ARM64, RISC-V ports.
 *
 * 3. Use an updated compiler. Sprite originally used a now obsolete version of GCC.
 * Sprite2 now uses Clang.
 *
 * 4. Cross-platform build. Right now, only Linux/macOS users can build Sprite. We want at some point
 * to support Microsoft Windows.
 *
 * 5. A Sprite userspace port. Some of Sprite's technologies can be ported as a userspace library and allow
 * for some applications to be ported to other applications.
 */

#include <sprite/sprite.h>

static void Init _ARGS_((void));

void
main(void)
{
	// Initialize variables unique for each architecture.
	Main_InitVars();

	// Initialize machine-dependent info.
	// IT MUST BE CALLED HERE!
	Mach_Init();
	Sync_Init();

	// Initialize the debugger.
	Dbg_Init();

	// Inform the debugger that we are booting up.
	Mach_MonPrintf("Sprite kernel for %s Built on %s at %s\n", machTarget, __DATE__, __TIME__);

	// Initialize the system module.
	Sys_Init();

	// Perform a partial VM initialization.
	// This allows for memory to be allocated via Vm_BootAlloc().
	// After Vm_Init(), which performs full VM initialization, then the
	// normal memory allocator can be used.
	Vm_BootInit();

	// Initialize all devices.
	Dev_Init();

	// Initialize system dump routines.
	Dump_Init();

	// Initialize process table.
	Proc_Init();

	// Initialize sync module.
	Sync_LockStatInit();

	// Initialize system timer.
	Timer_Init();

	// Initialize signal module.
	Sig_Init();

	// Initialize the scheduler.
	Sched_Init();

	// We cannot use printf() before this point.
	main_PanicOK++;
	printf("Sprite kernel: %s\n", SpriteVersion());

	// Set up bins for the memory allocator.
	Fs_Bin();
	Net_Bin();

	// Perform full VM initialization.
	// After this point, the normal memory allocator can and must be used.
	// Using Vm_BootAlloc() after this point will result in a kernel panic.
	Vm_Init();

	// Initialize the main process.
	Proc_InitMainProc();

	// Initialize networking.
	// We could move this call earlier, but Vm_Init() needs to run first.
	// However, we could update VM to move this call earlier.
	Net_Init();
	Net_RouteInit();

	// Enable server process manager.
	Proc_ServerInit();

	// Initialize the recovery module.
	Recov_Init();

	// Initialize RPC.
	Rpc_Init();

	// Configure devices that may or may not exist.
	Dev_Config();

	// Initialize profiling.
	Prof_Init();

	// Allow interrupts from now on.
	Mach_MonStartNmi();
	ENABLE_INTR();

	// Initialize fs recovery.
	Fsrecov_InitState();

	// Initialize dir op log recovery.
	Fsrecov_DirOpInit();

	// Sleep for a few seconds to calibrate the idle time ticks.
	Sched_TimeTicks();

	// Start profiling.
	(void) Prof_Start();

	// Do an initial RPC to get a boot timestamp.
	Rpc_Start();

	// Initialize the file system module.
	Fs_Init();

	// Get a current directory for the main process.
	Fs_ProcInit();

	// Start clock daemon.
	Proc_CallFunc(Vm_Clock, (ClientData) NIL, 0);
	Proc_CallFunc(Vm_OpenSwapDirectory, (ClientData) NIL, 0);

	// Start the process that synchronizes the filesystem caches with the data kept on disk.
	Proc_CallFunc(Fsutil_SyncProc, (ClientData) NIL, 0);

	// Create a few RPC server processes and the Rpc_Daemon process which will create
	// more server processes if needed.
	if (main_NumRpcServers > 0) {
		for (i = 0; i < main_NumRpcServers; i++)
			(void) Rpc_CreateServer((int *)&pid);
	}

	(void) Proc_NewProc((Address) Rpc_Daemon, PROC_KERNEL, FALSE, &pid, "Rpc_Daemon", FALSE);

	// Create processes to execute functions.
	(void) Proc_ServerProcCreate(FSCACHE_MAX_CLEANER_PROCS + VM_MAX_PAGE_OUT_PROCS);

	/*
	 * Create a recovery process to monitor other hosts.
	 * Can't use Proc_CallFunc's to do this because they can be used up waiting
	 * for page faults against down servers. (Alternatively the VM code could be
	 * fixed up to retry page faults later instead of letting the Proc_ServerProc
	 * wait for recovery.)
	 */
	(void) Proc_NewProc((Address) Recov_Proc, PROC_KERNEL, FALSE, &pid, "Recov_Proc", FALSE);

	// Set up process migration recovery management.
	Proc_MigInit();

	// Call the routine to start test kernel processes.
	Main_HookRoutine();

	printf("MEMORY %d bytes allocated for kernel\n", vmMemEnd - mach_KernStart);

	// Start up the first user process.
	(void) Proc_NewProc((Address) Init, PROC_KERNEL, FALSE, &pid, "Init", FALSE);
	(void) Sync_WaitTime(time_OneYear);
	Mach_MonPrintf("Leaving main()\n");
	Proc_Exit(0);
}

static void
Init(void)
{
	ReturnStatus status;
	char *initArgs[10];
	char argBuffer[100];
	char bootCommand[103];
	char *ptr;
	int argc, i, argLength;
	Fs_Stream *dummy;

	bzero(bootCommand, sizeof(bootCommand));
	argc = Mach_GetBootArgs(8, 100, &(initArgs[2]), argBuffer);
	if (argc > 0)
		argLength = (((int) initArgs[argc+1]) + strlen(initArgs[argc+1]) + 1 - ((int) argBuffer));
	else
		argLength = 0;

	if (argLength > 0) {
		initArgs[1] = "-b";
		ptr = bootCommand;
		for (i = 0; i < argLength; i++) {
			if (argBuffer[i] == '\0')
				*ptr++ = ' ';
			else
				 *ptr++ = argBuffer[i];
		}
		bootCommand[argLength] = '\0';
		initArgs[2] = bootCommand;
		initArgs[argc + 2] = (char *) NIL;
	} else
		initArgs[1] = (char *) NIL;

	if (main_AltInit != 0) {
		initArgs[0] = main_AltInit;
		printf("Execing \"%s\"\n", initArgs[0]);
		status = Proc_KernExec(initArgs[0], initArgs);
		printf("Init: Could not exec %s status %x.\n", initArgs[0], status);
	}

	status = Fs_Open(INIT, FS_EXECUTE | FS_FOLLOW, FS_FILE, 0, &dummy);
	if (status != SUCCESS)
		printf("Can't open %s <0x%x>\n", INIT,status);

	initArgs[0] = INIT;
	status = Proc_KernExec(initArgs[0], initArgs);
	printf("Init: Could not exec %s status %x.\n", initArgs[0], status);
	Proc_Exit(1);
}

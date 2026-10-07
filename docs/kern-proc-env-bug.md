# FreeBSD: `kern.proc.env` fails with ENOMEM for some processes

Found while debugging tposd on a ThinkPad A475 with FreeBSD 15.1-RELEASE-p4
(amd64, GENERIC), 2026-10-07.

**Status:** not reported upstream yet. To do: file a bug at
<https://bugs.freebsd.org/> with the reproduction and analysis below.

## Symptom

Reading another process's environment fails for a few percent of processes:
```
$ procstat -e $(pgrep -x tposd)
procstat: sysctl(kern.proc.env): Cannot allocate memory
  PID COMM             ENVIRONMENT
41375 tposd            -
```
The process itself is fine: its own `environ` is complete, and the strings on
its stack (where `ps_strings` points) are intact when read with gdb.

Consequence on the desktop: ConsoleKit2 finds the session of a process by
reading `XDG_SESSION_COOKIE` from its environment. When the read fails, polkit
does not consider the process part of the active session:
```
$ pkcheck --action-id org.tposd.mute-led --process $(pgrep -x tposd)
Not authorized.
```
In the session where this was found, both `tposd` and `Thunar --daemon`
(started by xfce4-session's autostart) were affected; the other session
processes were not.

## Reproduction

Needs nothing but the base system:
```sh
#!/bin/sh
# Start a process 200 times and count how often its environment can not
# be read.  procstat exits 0 even on this error, so look at the message.
n=0
for i in $(seq 200); do
	env -i A=1 /bin/sleep 5 &
	pid=$!
	sleep 0.03
	procstat -e $pid 2>&1 | grep -q "Cannot allocate" && n=$((n + 1))
	kill $pid
done
echo "failed: $n/200"
```
Results on the A475:

| | failed |
|---|---|
| ASLR on (default, `kern.elf64.aslr.enable=1`, `kern.elf64.aslr.stack=1`) | 4–9 / 200 |
| ASLR off for the child (`proccontrol -m aslr -s disable env -i A=1 /bin/sleep 5`) | 0 / 200 |

Whether a process is affected is decided at exec time; it stays so for its
whole life.

## Analysis

`sysctl_kern_proc_env()` → `proc_getenvv()` → `get_ps_strings()`
(`sys/kern/kern_proc.c`) reads every environment string in chunks of
`GET_PS_STRINGS_CHUNK_SZ` (256) bytes with `proc_read_string()`:
```c
static int
proc_read_string(struct thread *td, struct vmspace *vm, const char *sptr,
    char *buf, size_t len)
{
	ssize_t n;

	/*
	 * This may return a short read if the string is shorter than the chunk
	 * and is aligned at the end of the page, and the following page is not
	 * mapped.
	 */
	n = vmspace_iop(td, vm, (vm_offset_t)sptr, buf, len, UIO_READ);
	if (n <= 0)
		return (ENOMEM);
	return (0);
}
```
The comment expects a short read, but `vmspace_iop()`
(`sys/kern/sys_process.c`) returns -1 whenever `vmspace_rwmem()` reports an
error, even after part of the data was copied:
```c
	error = vmspace_rwmem(vm, &uio);
	if (error != 0 || uio.uio_resid == slen)
		return (-1);
	return (slen - uio.uio_resid);
```
The environment strings are the last strings below `ps_strings` at the top of
the main stack; above the stack top nothing is mapped. When the last string
starts less than 256 bytes below the stack top, its chunk runs past the top,
`vmspace_rwmem()` fails with EFAULT, and the whole sysctl returns ENOMEM.

With ASLR the stack top is placed with a random gap, so the distance varies
per exec. Measured with gdb (`find` for the string `A=1` below the top of the
mapping marked `D` in `procstat -v`):

| process | last env string starts | `procstat -e` |
|---|---|---|
| failing | 0xe2 (226) bytes below the stack top | ENOMEM |
| working | 0x7fa (2042) bytes below the stack top | `A=1` |

For tposd: `ps_strings` at stack top − 0x28, `DISPLAY=:0.0` (the last string)
169 bytes below the top.

`kern.proc.args` is not affected in practice: it is answered from the cached
`p_args` when present.

## Possible fixes (in the kernel)

- In `vmspace_iop()`: return the number of bytes copied when some were,
  as the comment in `proc_read_string()` assumes. Other callers would need
  checking.
- Or in `proc_read_string()` / `get_ps_strings()`: never read past the end of
  the page (or of the stack mapping), i.e. clamp the chunk to
  `PAGE_SIZE - (sptr & PAGE_MASK)` and continue on the next page, taking care
  that a chunk cut short at a page boundary is not taken as the end of the
  string.

## Effect on tposd

tposd sets the speaker mute LED through pkexec, because the speaker mute key
sends no event that devd could act on: the embedded controller handles it and
only `dev.acpi_ibm.0.mute` changes (checked on the A475 by listening on
`/var/run/devd.pipe`: the mic-mute key sends `notify=0x1b`, the speaker mute
key nothing). If tposd is one of the affected processes, polkit refuses it and
the LED does not follow the key for that session. Logging out and in again
gives it a new chance.

The mic-mute key does send an ACPI event and is handled by devd(8)
(`etc/devd/tposd.conf`, `libexec/tposd-key`), which runs as root and is not
affected.

tposd logs it at start (`grep tposd /var/log/messages`):
```
tposd[PID]: the kernel can not read this process's environment (kern.proc.env: ENOMEM, ...)
```

### Not done yet: re-exec

tposd could check at start whether `kern.proc.env` of its own pid fails with
ENOMEM and, if so, `execv()` itself once more: the new exec gets a new random
stack gap, and in ~95% of cases a readable environment. Left out for now
because it is a workaround for a kernel bug and hard to test (it needs an
affected start). Do it if the log line above shows up in practice.

<p align="center">
  <a href="https://opensource.org/licenses/MIT"><img src="https://img.shields.io/badge/License-MIT-yellow.svg" alt="License: MIT"></a>
  <a href="https://wikipedia.org/wiki/C_(programming_language)"><img src="https://img.shields.io/badge/language-C-blue.svg" alt="Language: C"></a>
  <img src="https://img.shields.io/badge/platform-cross--platform-lightgrey.svg" alt="Platform: Cross-Platform">
</p>

# Epic! - A full-system Intel IA-64 (Itanium) emulator.

Epic! is a from-scratch emulator for the Intel IA-64 (Itanium) architecture, written in C. It emulates a full system, including CPU, firmware (PAL/SAL/ACPI/EFI), interrupt controllers, PCI bus, storage, and boot devices - once this is fully complete, this will be enough to boot most operating systems.

It is inspired by trofi's fork of the HP-made "ski" simulator, however focuses on correctness, readability, and the ability to not rely on modified kernels.

As of now, it is not capable of booting any operating system completely. But, I am working on this.

### About this emulator
Epic! emulates an Intel Itanium 2-class (McKinley-like) processor with (pretty much) full IA-64 instruction set support.
*Full instruction set support EXCLUDES IA-32 compatibility mode and a small number of privileged instructions not required for OS boot.*

## Features
### Implemented (not a definitive list)

**CPU & architecture** - complete register state (GR, FR, PR, BR, AR, CR, RR, PKR, PMC/PMD), all 13 bundle templates, RSE with virtual-to-physical register mapping, ITR/DTR/ITC/DTC TLBs, ALAT, interruption delivery and full IVT, 61-bit VA/50-bit PA addressing.

**Instruction units** - integer ALU (ADD, SUB, CMP, DEP, EXTR, shifts, etc.), memory load/store (LD/ST 1–8, LDF/STF, semaphore ops), branches (BR.COND/CALL/RET, BRL, RFI, BSW), floating-point (FMA, FMS, FNMA, FCMP, FCVT, FRCPA, etc.).

**Hardware** - physical memory subsystem (RAM/ROM/MMIO), Local + I/O SAPIC interrupt controllers, PCI bus (host bridge, ISA bridge, PIIX4 IDE), IDE/ATAPI (PIO + DMA), UART 16550 (COM1/COM2), MC146818 RTC, PS/2 keyboard.

**Firmware & boot** - PAL 3.1, SAL 3.1, ACPI 2.0 (RSDP/XSDT/DSDT), UEFI 2.0 boot + runtime services, PE32+ loader, GPT, FAT32, ISO 9660 + El Torito, direct kernel boot, CD-ROM EFI boot, configurable RAM/SMP/serial, trace logging.

### In progress
- [ ] Network device
- [ ] Display output (Qt)
- [ ] Floppy disk boot
- [ ] IA-32 compatibility mode
- [ ] JIT recompilation - along way away!

## Usage
```
$ epic [options]

Options:
  -m <megs>       RAM size in MB (default: 2048)
  -smp <n>        Number of CPUs (default: 1)
  -cdrom <iso>    Boot from ISO image
  -fda <img>      Boot from floppy image
  -hda <img>      Hard disk image
  -bios <path>    Custom firmware path
  -serial <mode>  Serial backend: stdio, file:<path>, none
  -cmdline <str>  Override the Linux kernel command line
  -kernel <path>  Direct-boot an IA-64 kernel (PE32+ vmlinuz)
  -initrd <path>  Initrd file (for -kernel boot)
  -trace          Enable trace-level logging
  -test           Run validation tests
  -dump-state     Dump CPU/TLB/RSE state after execution
  -help           Show this help
```

### Examples

**Boot T2 SDE from ISO:**
```bash
\$ epic -cdrom t2-26.6-ia64-desktop-glibc-gcc-itanium2.iso -serial stdio -trace
```

**Direct kernel boot:**
```bash
\$ epic -kernel vmlinuz-7.0.10-t2 -initrd initrd-7.0.10-t2 -cmdline "console=ttyS0 root=/dev/sda2"
```

**Run validation tests:**
```bash
\$ epic -test
```

### Building & testing

```sh
# Build the project
mkdir build && cd build
cmake ..
cmake --build .
```

On Windows: `cmake -G "Ninja" ..`  (ninja is recommended). CMake 3.16+ and a C compiler (MSVC, GCC, or Clang) are required.
This hasn't been tested on any other environment other than the latest Windows 11 Insider build, so expect issues.

## Contributing
Contributions are welcome, particularly in these areas:
- Testing: if you have access to disc images, testing and filing detailed bug reports is enormously valuable. Describe what you tried, what output you got, and ideally attach a -trace log.
- Bug fixes: see the open issues list for any known problems.
- Documentation: the IA-64 architecture is poorly documented outside the official SDM. If you know the architecture well and spot anything wrong or missing in the code comments, fixes are warmly welcomed!
- Device emulation: network device backend and display output (Qt) are the next major (hardware) targets.
  
_Please open an issue before starting significant work so we can discuss it - this is a solo project with strong design opinions & I'd hate for anyone to waste effort on something that won't merge!!_

## Credits

This project is inspired by and builds upon concepts from:
- **trofi's [SKI fork](https://github.com/trofi/ski)**
- The **IA-64 Architecture Software Developer's Manual** (Intel)
- The **UEFI 2.x Specification**
- The **[T2 SDE Project](https://github.com/rxrbln/t2sde)** for providing minimal and up-to-date bootable images

## Transparency - usage of AI in this project
I like to be totally and utterly transparent about this matter, in an age where it is difficult to tell what is human and what is generated. I think it's important to realise AI in programming shouldn't replace the human, but assist. I firmly believe AI should be used as a tool; however, conversely, I don't mind if people want to use AI to so-called "vibecode" things as long as they acknowledge this. 

Anyway, in terms of the relation of this project and the subject of AI: I have used it not to do the work for me, but to **_ASSIST_** me with tasks that make me want to rip my hair out such as debugging hundreds of millions of lines of debug output (truly!!), and implementing boot services, as that is something I am not very knowledgable about. It hasn't replaced my work though. Furthermore, at several points, it has been simply invaluable with explaining various concepts, whether it be about the inner workings of the architecture or some weird quirk with C. 

If you have a problem with it, good for you, I don't care. It's 2026.

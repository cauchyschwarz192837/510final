# Operating Systems (CS510) Final Project: Inter-Process Communication Through mmap(), munmap() & GPU Driver in xv6

See the file "510 Project Writeup (Group 13).pdf" for details

Designed and implemented a full mmap() / munmap() subsystem in the xv6 (RISC-V) kernel by extending the virtual
memory manager and page-fault handler to support demand paging and file-backed mappings. Introduced a per-process
VMA table to track virtual address ranges, permissions, offsets, and backing inodes. Modified the trap handler to detect
load/store page faults, allocate physical pages lazily, and install PTEs with correct RISC-V permission bits
(PTE R/W/X/U/V)

Implemented MAP PRIVATE copy-on-write (COW) semantics by clearing PTE W, using a reserved software-defined
PTE bit to mark COW pages, and maintaining per-page reference counts. On write faults, performed page duplication,
updated page tables, and ensured TLB coherence. Implemented MAP SHARED semantics with correct dirty-page
write-back to disk during partial munmap() and exec(), integrating with the buffer cache and inode layer. Extended
fork() system call to duplicate VMAs and propagate COW mappings without eager copying

Demonstrated inter-process communication by designing a multi-process Pong game rendered in the command line
interface. Began implementing a virtio-GPU framebuffer driver to render the Pong game in a window

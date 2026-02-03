See 510 Project Writeup (Group 13).pdf for details

-------------------------------------------------------------------------------------------------------------------

vm grows from trapframe to low addr
```c
//proc.h
struct mapped_region {
  uint64 start_address;        // start virtual address
  uint64 end_address;          // end virtual address  calculate sz
  int prot; // virtual memory permission
  int flags; //mark whether the modifications to the mapped memory are written back to the file
  int offset; // start point of the mapping file
  int in_use;
  struct file *mapped_file;    // The VMA should contain a pointer to a struct file for the file being map
};
//per proc state
struct proc{
...
struct mapped_region mapped_regions[MAPNUM];      // arbitrary number 16
uint64 mapped_region_top;  //free virtual address start
int o_sz; //original heap size before proc calls mmap
}
```
mmap sys call 
```c
//sysfile.c
//why don't read offsite 
uint64 sys_mmap(void)
{ 
//argfd(5, &offsite) < 0 但是其实在read_mapping可以读取出来
// if ((!f->readable && (prot & PROT_READ))
//    || (!f->writable && (prot & PROT_WRITE) && !(flags & MAP_PRIVATE)))
}

```
fcntl.h 
```c
//maybe not necessary
#define PROT_NONE   0x0
#define PROT_EXEC   0x4
```


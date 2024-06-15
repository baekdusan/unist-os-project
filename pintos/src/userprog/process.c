#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
void argument_stack(char **argv, int argc, void **esp);

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) //argument passing 이 함수 고치기
{
  char *fn_copy;
  tid_t tid;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE); //문자열 복사 filename을 fn_copy로 복사함.

  // //
  // //argv,argc를 넘기는 것 때문에 start_process에서 parsing을 하려고 햇는데 그렇게 하면 여기 새로운 thread의 이름이 이상해짐. file이름이 아니게됨.
  // char *delim = " ";  // 구분자는 공백, 쉼표, 점
  // char *saveptr;
  // file_name = strtok_r(file_name, delim, &saveptr); //file_name 에서 가장 첫번쨰 토큰을 뽑아냄. 추후 therad name으로 사용.
  // //위 strtok_r에 주소 넣으면 그 주소 내용 변경된다. 그래서 file_name을 넣어줌.
  // printf("first token file_name: %s\n", file_name);
  // //
  char filename_copy[128];
  strlcpy(filename_copy, file_name, strlen(file_name)+1);
  char *save_ptr, *token;
  token = strtok_r(filename_copy, " ", &save_ptr);
  struct file *file = NULL;
  file = filesys_open(token);
  if(file==NULL){ //만약 파일이 존재하지 않으면 fail
    file_close(file);
    return -1;
  }

  /* Create a new thread to execute FILE_NAME. */
  //현재 구현의 문제점은 command line을 parse하지 않고 그대로 넘겨준다는 것임.
  // tid = thread_create (file_name, PRI_DEFAULT, start_process, fn_copy); // 새로운 프로그램 실행할 쓰레드 만듬.
  tid = thread_create (token, PRI_DEFAULT, start_process, fn_copy);
  sema_down(&thread_current()->exec_sema); //이걸로 parent가 child가 load를 다 했는지 확인함. child가 load를 다 했으면 parent가 다음으로 넘어갈 수 있게 해줌.

  //그리고 새롭게 만들어진 쓰레드가 start_process를 우선 실행하게 하네. start process는 load하고 원하는 프로그램 실행하게 하는 function.
  if (tid == TID_ERROR)
    palloc_free_page (fn_copy); 
  return tid;
}


/* A thread function that loads a user process and starts it
   running. */ //interrupt frame을
static void
start_process (void *file_name_) //argument passing 이 함수 고치기
{
  char *file_name = file_name_; //comand line임.
  struct intr_frame if_; //interrupt frame선언. kernel space에 있는 user param을 저장하는 stack임.
  bool success;
  // ///argument parsing부분 구현 argv와 argc를 strtok_r을 이요해서 구현
  // char *token, *save_ptr;
  // char *argv[128]; //document에 따르면, kernel에 pass하는데에 128byte limit이 있다고 함.
  // int argc = 0; //strtok_r은 문자열을 받아서 해당 문자열을 기준대로 나눠서 나눈 문자열의 시작 포인터를 반환해줌. 그래서 원본 문자열 free되면 접근이 안됨.
  // //아 동작방식이 원본 문자열에서 기준으로 받은 것들을 다 \0으로 바꾸는 것이 동작 방식이라고 함.
  // for(token = strtok_r(file_name, " ", &save_ptr); token != NULL; token = strtok_r(NULL, " ", &save_ptr)){
  //     argv[argc] = token;
  //     argc++;
  // }
  //그래서 이 과정이 끝나면 argv에는 각 argument의 '시작 주소'가 들어가게 된다.
  ///

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  //기존 file_name대신 argv[0]을 넣어줘서 실행할 프로그램의 이름을 넣음. 이로써 load는 이 파일을 executable을 찾아서 mem에 load한다.
  success = load (file_name, &if_.eip, &if_.esp);  //disk에 있는 binary file을 mem에 올린다.
  //pintos에서는 load함수가 file name을 받아서 해당 file을 disk에서 mem으로 옮기고, 실행되어야 할 instruction을 eip에 저장한다.
  //그리고 user stack의 top을 esp에 저장해준다.
  //if_.esp는 address of the top of the user stack. eip는 location of instruction.
  //user stack init해주고 esp, eip 설정해준다.

  palloc_free_page (file_name);

  sema_up(&thread_current()->parent->exec_sema); //아까 process exec에서 child load완료 대기중.
  if (!success)
  {
    thread_current()->load_success = 0; //load 실패 추후 -1반환에 쓰임.
    thread_exit ();
  } 

//  /* If load failed, quit. */ ///이 부분 argv를 포인터 배열로 하면 free page를 하기 때문에 받아오지를 못함. 그래서 변경해야함.
//  palloc_free_page (file_name);
//  if (!success)
//    thread_exit ();
  /* 이 부분에 stack을 setup하는 부분을 구현해야 한다*/
  //user stack을 initalize하고 argument를 stack에 push하는 부분을 구현하면 됨.
  //user program을 실행할 때, 우리는 argument를 pass 해줘야 한다. 이 부분을 구현해주면 된다.
  //즉, 이 부분에 stack을 setup하는 함수를 넣으면 됨. 이렇게 넣으면 user mode로 돌아갔을 때 stack 으로부터 pop을 통해 argument를 가져간다.
    //if_.esp가 user stack의 top을 가리키고 있따.
  // argument_stack(argv, argc, &if_.esp); //esp pointer의 pointer를 넣음.
  // hex_dump(if_.esp, if_.esp, PHYS_BASE - if_.esp, true); //stack의 내용을 출력해준다.
  // palloc_free_page (file_name);
  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory"); //intr_exit을 실행하여 user program으로 넘어간다.
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid UNUSED) 
{
  // for(int i=0; i< 1000000000; i++){
    
  // }
  struct thread *child = NULL;
  struct list_elem *e; //내 자식인지 check
  for(e = list_begin(&thread_current()->child_list); e != list_end(&thread_current()->child_list); e = list_next(e)){
    struct thread *t = list_entry(e, struct thread, child_elem);
    if(t->tid == child_tid){
      child = t;
      break;
    }
  }

  if(child == NULL){ //내 자식이 아닌거 wait하라 했으니까 err
    return -1;
  }

  sema_down(&child->wait_sema); //child가 종료될 때까지 기다림.
  list_remove(&child->child_elem); //child list에서 제거해줌.
  int status = child->exit_status;
  sema_up(&child->exec_sema); //child가 제거될 수 있게 해줌.
  return status; //자식 pid반환.
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;

  //exit을 할 때도, 만약에 자식이 있는 상태로 exit하면 orphan이 생기니까 이렇게 자식들 끝나는거 기다려줌.
  struct list_elem *child;
  for(child = list_begin(&cur->child_list); child != list_end(&cur->child_list); child = list_next(child)){
    struct thread *child_thread = list_entry(child, struct thread, child_elem);
    process_wait(child_thread->tid);  //그리고 이렇게 하는게 죽은 자식들 회수도 하는 것임.
  }
  file_close(cur->exec_file);
  sema_up(&cur->wait_sema);
  sema_down(&cur->exec_sema);

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP //executable에 들어가는 주소구나 starting entry point of function이라고 설명해줌.
   and its initial stack pointer into *ESP. //stack top of user stack이라고 설명해줌.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create (); //page directory 만듬. Pagetable로의 pointer라고 함.
  if (t->pagedir == NULL) 
    goto done;
  process_activate (); //set cr3 register라는데 cr3 register가 뭔지 모르겠음.

  // ///argument parsing부분 구현 argv와 argc를 strtok_r을 이요해서 구현
  char *token, *save_ptr;
  char *argv[128]; //document에 따르면, kernel에 pass하는데에 128byte limit이 있다고 함.
  int argc = 0; //strtok_r은 문자열을 받아서 해당 문자열을 기준대로 나눠서 나눈 문자열의 시작 포인터를 반환해줌. 그래서 원본 문자열 free되면 접근이 안됨.
  //아 동작방식이 원본 문자열에서 기준으로 받은 것들을 다 \0으로 바꾸는 것이 동작 방식이라고 함.
  char filename_copy[128]; //strtok원본 문자열 나누는거라 복사해줘야함.
  strlcpy(filename_copy, file_name, strlen(file_name)+1);
  for(token = strtok_r(filename_copy, " ", &save_ptr); token != NULL; token = strtok_r(NULL, " ", &save_ptr)){
      argv[argc] = token;
      argc++;
  }

  char* file_copy2 = argv[0];

  /* Open executable file. */
  file = filesys_open (file_copy2);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }

  /* Read and verify executable header. */
  //elf file을 parse하고 ELF header를 얻음. header에 다른 segment들 어디있고 이런 정보가 있음.
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  /* Read program headers. */ //load segment info
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page, //load the executable file이라고 함.
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp)) //user stack을 init하는 부분.
    goto done;

  argument_stack(argv, argc, esp);

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry; //실행하고자 하는 instruction의 시작 주소를 eip에 저장.

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  //// file_close (file);
  if(success == 1){ //성공하면 여기에 쓴다 그리고 막아줌
    t->exec_file = file;
    file_deny_write(file);
  }
  return success;
}


void argument_stack(char *argv[], int argc, void **esp){
  size_t len;
  int arg_bytes = 0;
  char *argv_addr[argc]; //argument를 넣어준 이후에는 각 argument가 저장된 주소를 넣어줘야해서 이거 유지해야함.
  //user stack에 argument를 넣음.
  for(int i = argc -1; i >= 0; i--){
    len = strlen(argv[i]) + 1; // '/0'을 넣어줘야 하니까. +1해서 넣어줌
    arg_bytes += len;
    *esp -= len; //stack은 높은 주소에서 내려가니까.
    memcpy(*esp, argv[i], len); //argument를 stack에 넣어줌.
    //argv[i]는 해당 argument의 시작주소이다. 그래서 memcpy로 해당 시작주소가 가르키는 값에서 len만큼을 복사해줌. 그럼 tok으로 나눌 때 \0들어가있으니까 len+1로하면 그것까지 됨.
    //그래서 이제는 file name이 free되어도 상관 없음.
    argv_addr[i] = *esp; //argument의 주소를 저장해줌.
  }
  //convention에 이 argument가 4byte에 align되어 있어야 한다고 함. 0으로 align해줌.
  while(arg_bytes % 4 != 0){
    *esp -= 1;
    *((uint8_t*)*esp) = 0; //Esp는 esp의 pointer의 pointer임. 그래서 *로 한번 참조하면 esp가 됨. 그걸 uint8_t로 형변환함. 1byte를 넣기 위해서.
    //그래서 *로 한번 더 해당 주소를 참조하면 esp의 값인거니까 거기를 0으로 넣어주면 됨.
    arg_bytes++;
  }
//  uint32_t padding = 4 - ((uintptr_t)*esp % 4); //argument byte기준인지 esp기준인지 잘 모르겠음
//  while(padding != 0){
//    *esp -= 1;
//    *((uint8_t*)*esp) = 0; //1byte넣기.
//    padding--;
//  }
  //Null terminate string을 넣어서 argument다 넣은거 표시
  *esp -= sizeof(char*); //4byte
  *((char**)*esp) = 0; //char*로 넣도록 하기 위해 이렇게 해줌.
  //argument의 주소를 넣어줌.
  for(int i = argc -1; i >= 0; i--){
    *esp -= sizeof(char*);
    *((char**)*esp) = argv_addr[i];
  }
  //argv의 주소를 넣어줌.
  char **argv_start = (char**)*esp;
  *esp -= sizeof(char**); //type이 char**임.
  *((char***)*esp) = argv_start;
  //argc를 넣어줌.
  *esp -= sizeof(int);
  *((int*)*esp) = argc;
  //return address를 넣어줌.
  *esp -= sizeof(void*);
  *((void**)*esp) = 0;
}


/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Get a page of memory. */
      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      /* Load this page. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false; 
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* Add the page to the process's address space. */
      if (!install_page (upage, kpage, writable)) 
        {
          palloc_free_page (kpage);
          return false; 
        }

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp) 
{
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL) 
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success)
        *esp = PHYS_BASE;
      else
        palloc_free_page (kpage);
    }
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
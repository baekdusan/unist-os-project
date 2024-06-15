#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/synch.h"
#include "userprog/pagedir.h"
#include "devices/shutdown.h"
#include <string.h>

static void syscall_handler (struct intr_frame *);
bool mycreate(const char *file, unsigned initial_size);
bool myremove(const char *file);
int mywrite(int fd, const void *buffer, unsigned size);
int myopen(const char *file);
int myread(int fd, void *buffer, unsigned size);
void myexit(int status);
int mytell(int fd);
void myclose(int fd);
int myexec(const char *cmdline);
void ptr_valid_check(void *ptr);

struct lock global_lock;

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init(&global_lock);
  // sema_init(&(thread_current() -> sema_exec), 0);
}

//invalid한 경우는, user가 null ptr을 주었거나, VM에 unmapped된걸 주엇거나, kernel space를 가르키거나이다.
//pagedir_get_page()을 이용해서 접근함으로써 간접적으로 page fault를 일으킨다.

void ptr_valid_check(void *ptr){ //pagedir.c, vaddr.h의 함수 사용.
    if(ptr == NULL || pagedir_get_page(thread_current()->pagedir, ptr) == NULL || !is_user_vaddr(ptr)){
//        thread_exit(); //이거 잘 처리해야 할 것 같은데, 왜냐하면 나중에 process가 exit으로 종료되지 않고 kernel에 의해 종료시 -1을 반환하게 만들라고 함.
        //exit status만 잘 정해주면 되나?
        myexit(-1);
    }
}

//user program이 write등과 같은 syscall을 발생시면, lib에 있는 syscall에서 syscall3같은 함수를 호출.
//위 함수의 의미는 3개의 parameter를 push하고 int 0x30을 실행시킴. int 0x30같은 경우는 interrupt vector table의 0x30위치에 syscall_handler()가 있으므로
//이 밑의 syscall_handler가 실행된다. 그러므로 우리는 이 밑의 syscall handler function을 채우면 되는 것이다.
//이거 진짜로 intr frame인가봄. interrupt.h에 struct가 있는데 f->esp이런식으로 필요한거 사용 가능. int가 끝나고 온 것이어서 user의 register들은 들어있을 것.
static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  ptr_valid_check(f->esp); //user stack top valid check
//f->esp는 user stack top을 가르킴. 그래서 다음 arg같은거 읽어올 때, +4 해서 주소 가져와서 읽으면 됨.
//가져온 주소는 user stack top의 다음 주소일 테니까.
  int syscall_num = *(int *)(f->esp); //syscall number를 가져옴.
  // printf("syscall number: %d\n", syscall_num);
    switch(syscall_num) { //user에서 푸시할 때, 마지막에 syscall number를 push했으므로 user stack의 esp로 int만큼 읽으면 number이다.
      case SYS_HALT:
        printf("SYS_HALT\n");
        shutdown_power_off();
        break;
      case SYS_EXIT:
        // printf("SYS_EXIT\n");
        ptr_valid_check(f->esp + 4); //1개의 argument를 가르키는 stack ptr valid check. stack의 esp +4위치에 arg존재.
        int status = *(uint32_t*)(f -> esp + 4);
        myexit(status);
        break;
      case SYS_EXEC: // cmd line 한개의 argument.
        printf("SYS_EXEC\n");
        // ptr_valid_check(f -> esp + 4);
        // const char *cmdline = *(uint32_t*)(f -> esp + 4);
        // ptr_valid_check(cmdline);
        // f -> eax = myexec(cmdline);
        break;
      case SYS_WAIT: // pid 하나만 받음.
        printf("SYS_WAIT\n");
        // ptr_valid_check(f -> esp + 4);
        // int pid = *(uint32_t*)(f -> esp + 4);
        // f -> eax = process_wait(pid);
        break;
      case SYS_CREATE: //create는 argument가 2개임. 앞에거는 file name, 뒤에거는 initsize
        printf("SYS_CREATE\n");
        ptr_valid_check(f -> esp + 4);
        ptr_valid_check(f -> esp + 8);
        const char *file_name = *(uint32_t*)(f -> esp + 4);
        unsigned init_size = *(uint32_t*)(f -> esp + 8);
        // ptr_valid_check(file_name);
        f -> eax = mycreate(file_name, init_size);
        break;
      case SYS_REMOVE: //remove는 argument하나이다. file name
        printf("SYS_REMOVE\n");
        ptr_valid_check(f -> esp + 4);
        const char *file_name1 = *(uint32_t*)(f -> esp + 4);
        // ptr_valid_check(file_name1);
        f -> eax = myremove(file_name1);
        break;
      case SYS_OPEN: //open은 file name 하나만 받는다.
        // printf("SYS_OPEN\n");
        ptr_valid_check(f-> esp + 4);
        const char *file_name2 = *(uint32_t*)(f->esp + 4);
        // ptr_valid_check(file_name2);
        f -> eax = myopen(file_name2);
        break;
      case SYS_FILESIZE: //filesize는 fd하나만 받는다.
        printf("SYS_FILESIZE\n");
        // ptr_valid_check(f -> esp + 4);
        // int fd = *(uint32_t*)(f -> esp + 4);
        // f -> eax = file_length(thread_current()->file_descriptor[fd]);
        break;
      case SYS_READ: // read 는 argument 총 3개 받음. fd, buffer ptr, size
        printf("SYS_READ\n");
        // ptr_valid_check(f-> esp + 4);
        // ptr_valid_check(f-> esp + 8);
        // ptr_valid_check(f-> esp + 12);
        // int fd2 = *(uint32_t*)(f->esp + 4);
        // void *buffer2 = *(uint32_t*)(f->esp + 8);
        // unsigned size2 = *(uint32_t*)(f->esp + 12);
        // ptr_valid_check(buffer2);
        // f -> eax = myread(fd2, buffer2, size2);
        break;
      case SYS_WRITE: //write는 3개의 argument를 받는다. fd, buffer ptr, size
        // printf("SYS_WRITE\n");
        ptr_valid_check(f-> esp + 4);
        ptr_valid_check(f-> esp + 8);
        ptr_valid_check(f-> esp + 12);
        int fd1 = *(uint32_t*)(f->esp + 4);
        const void *buffer = *(uint32_t*)(f->esp + 8);
        unsigned size = *(uint32_t*)(f->esp + 12);
        ptr_valid_check(buffer);
        f -> eax = mywrite(fd1, buffer, size);
        break;
      case SYS_SEEK: //seek은 fd, position 두 개를 받음.
        printf("SYS_SEEK\n");
        // ptr_valid_check(f -> esp + 4);
        // ptr_valid_check(f -> esp + 8);
        // int fd3 = *(uint32_t*)(f -> esp + 4);
        // unsigned position = *(uint32_t*)(f -> esp + 8);
        // file_seek(thread_current()->file_descriptor[fd3], position);
        break;
      case SYS_TELL: //fd 하나만 받음.
        printf("SYS_TELL\n");
        // ptr_valid_check(f -> esp + 4);
        // int fd4 = *(uint32_t*)(f -> esp + 4);
        // f -> eax = mytell(fd4);
        break;
      case SYS_CLOSE: //fd 하나만 받음.
        printf("SYS_CLOSE\n");
        // ptr_valid_check(f -> esp + 4);
        // int fd5 = *(uint32_t*)(f -> esp + 4);
        // myclose(fd5);
        break;
      default:
        printf("system call!\n");
    }
  // printf ("system call!\n");
  // thread_exit ();
}

void myexit(int status){
    lock_acquire(&global_lock);
    struct thread *cur = thread_current();
    cur->exit_status = status;
    printf("%s: exit(%d)\n", cur->name, status);
    lock_release(&global_lock);
    for(int i = 2; i < 64; i++){
        if(cur->file_descriptor[i] != NULL){
            file_close(cur->file_descriptor[i]);
            cur -> file_descriptor[i] = NULL;
        }
    }
    thread_exit();
}

bool mycreate(const char *file, unsigned initial_size){
  // printf("File: %s\n", file);
    if(file == NULL){
        myexit(-1);
    }
    return filesys_create(file, initial_size);
}

bool myremove(const char *file){
    if(file == NULL){
        return false; //맞는지 모름.
    }
    return filesys_remove(file);
}

int mywrite(int fd, const void *buffer, unsigned size){
    struct thread *cur = thread_current();
    // printf("fd : %d\n", fd);
    lock_acquire(&global_lock);
    if(fd == 1){
        putbuf(buffer, size); //fd가 1 즉, stdout이면 putbuf에 쓴다.
        lock_release(&global_lock);
        // printf("here??\n");
        return size;
    }
    // if(fd < 2 || fd >= 64 || cur->file_descriptor[fd] == NULL){ //fd가 2 아래거나, 128이상이거나, 그리고 쓰려는 것에 NULL이면 오류.
    //     lock_release(&global_lock);
    //     return -1;
    // }
    printf("여기 와야 consol인데\n");
    int num = file_write(cur->file_descriptor[fd], buffer, size);
    lock_release(&global_lock);

    return num;
}

int myopen(const char *file){
    if(file == NULL || strchr(file, '\0') == NULL){ //file name이 NUll이거나 \0으로 안끝나면 못염.
        return -1; //open에서의 error를 반환해줌.
    }
    lock_acquire(&global_lock);
    struct file *f = filesys_open(file);
    if(f == NULL){ //file open 오류
        lock_release(&global_lock);
        return -1;
    }
    struct thread *cur = thread_current(); //정상인 경우, 빈 file descripter num에 할당하고 그 num을 반환.
    cur -> file_descriptor[cur->next_fd] = f;
    int fd = cur->next_fd;
    cur->next_fd++;
    lock_release(&global_lock);
    return fd;
}

// int myread(int fd, void *buffer, unsigned size){
//     struct thread *cur = thread_current();
//     lock_acquire(&global_lock);
//     if(fd == 0){ //key board에서 읽어옴.
//         uint8_t ch = input_getc();
//         lock_release(&global_lock);
//         return ch;
//     }
//     if(fd < 2 || fd >= 64 || cur->file_descriptor[fd] == NULL){ //file descriptor범위를 넘어섬.
//         lock_release(&global_lock);
//         return -1;
//     }
//     int num = file_read(cur->file_descriptor[fd], buffer, size); //아닌 경우 file에서 읽기.
//     lock_release(&global_lock);
//     return num;
// }

// int mytell(int fd){
//     if(thread_current()->file_descriptor[fd] == NULL){
//         return -1;
//     }
//     return file_tell(thread_current()->file_descriptor[fd]);
// }

// void myclose(int fd){
//     struct thread *cur = thread_current();
//     if (fd < 2 || fd >= 64 || cur->file_descriptor[fd] == NULL){
//         return;
//     }
//     file_close(cur->file_descriptor[fd]);
//     cur->file_descriptor[fd] = NULL;
// }

// int myexec(const char *cmdline){
//     if(cmdline == NULL){ //error check
//         return -1;
//     }
//     struct thread *check = NULL;
//     int pid = process_execute(cmdline);
//     if(pid == TID_ERROR){
//         return -1;
//     }
//     struct list_elem *e;
//     for(e = list_begin(&thread_current()->child_list); e != list_end(&thread_current()->child_list); e = list_next(e)){
//         struct thread *t = list_entry(e, struct thread, child_elem);
//         if(t->tid == pid){
//             check = t;
//             break;
//         }
//     }
//     if(check == NULL){ //child list에 추가되지 못한 경우.
//         return -1;
//     }
//     if(check->load_success == 0){ //load에 실패한 경우
//         return -1;
//     }
//     return pid;
// }

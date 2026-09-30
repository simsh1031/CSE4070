#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init (void);
void syscall_close_files (void);

int fibonacci (int n);
int max_of_four_int (int a, int b, int c, int d);

#endif /* userprog/syscall.h */

#pragma once
#define FILE_RD 1
void sys_dict_info(unsigned long *drv, char *id);
int sys_open(const char *p, int m);
int sys_close(int fd);
int sys_closeall(void);
int sys_seek(int fd, int pos, int whence);
int sys_read(int fd, void *buf, int len);

#ifndef _MOTOR_CTRL_H
#define _MOTOR_CTRL_H

#include <sys/ioctl.h>

/* IOCTL 魔法数 */
#define MOTOR_MAGIC 'M'

/* 控制命令 */
#define MOTOR_OPEN   _IO(MOTOR_MAGIC, 1)  /* 开门 */
#define MOTOR_CLOSE  _IO(MOTOR_MAGIC, 2)  /* 关门 */

#endif /* _MOTOR_CTRL_H */
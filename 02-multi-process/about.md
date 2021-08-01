# 多进程模型

1. 编译 `gcc webserver.c -o webserver -lpthread -lrt`
2. 运行：`./webserver 8181 myapp`，开启`8181`端口，工作目录在 myapp
3. 日志：记录`webserver`运行日志在 `myapp/nweb.log`, 记录子进程、所有子进程日志在 `myapp/timetest.log`
4. 使用`信号量、共享内存`统计 总时间（多线程之间）

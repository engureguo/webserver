# 单进程服务器

1. 编译 `gcc webserver.c -o webserver`
2. 运行 `./webserver 8181 myapp` (具体见 multi-process/readmeaboutthisdirectory.mdi)

# about

1. 单进程模型，效率低，快速请求会导致进程自动终止（为什么我还不知道）
2. webserver日志 `myapp/nweb.log`，具体用时则 显示在屏幕上


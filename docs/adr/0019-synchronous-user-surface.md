# 用户面只提供同步 API；pipeline、LISTEN、二进制格式只在线路面使用

两个使用方都在线程池里同步调用数据库（asio worker、httplib worker），所以用户面不提供异步 API，
也不公开 pipeline、COPY、LISTEN 或取消句柄。线路面照样使用 pipeline、LISTEN/NOTIFY 和二进制格式（0004、0006、0013）：
两个使用方“没用到”某个协议能力，不构成线路面不用它的理由。取消只由引擎在截止时间到达时内部使用；
关停时应该等请求结束，而不是打断它。

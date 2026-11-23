// 第10节：Go的echo服务器
//
// 用法：./echo_go [-addr :9000] [-stats]
//   -stats  每秒打印一次goroutine数和当前连接数（任务C用）
//
// 每个连接一个goroutine，写法是同步阻塞的：Read没数据就“阻塞”。
// 但在Go运行时里，fd是非阻塞的、用epoll边沿触发管理：read返回EAGAIN时，
// 挂起的是goroutine而不是线程，线程（M）可以去跑别的goroutine。

package main

import (
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"runtime"
	"sync/atomic"
	"time"
)

func main() {
	addr := flag.String("addr", ":9000", "监听地址")
	stats := flag.Bool("stats", false, "每秒打印 goroutine 数和连接数")
	flag.Parse()

	ln, err := net.Listen("tcp", *addr)
	if err != nil {
		log.Fatal(err)
	}
	log.Printf("监听 %s", ln.Addr())

	var nconn atomic.Int64 // 当前连接数，多个goroutine同时改，用原子变量

	if *stats {
		go func() {
			for range time.Tick(time.Second) {
				fmt.Printf("goroutine=%d 连接=%d\n", runtime.NumGoroutine(), nconn.Load())
			}
		}()
	}

	for {
		conn, err := ln.Accept()
		if err != nil {
			// 比如EMFILE（fd用完）：不能直接continue空转，打印并稍等一下再重试
			log.Printf("accept: %v", err)
			time.Sleep(5 * time.Millisecond)
			continue
		}
		nconn.Add(1)
		go func() {
			defer nconn.Add(-1)
			defer conn.Close()
			// 读到什么就写回什么，直到对方关闭或出错
			io.Copy(conn, conn)
		}()
	}
}

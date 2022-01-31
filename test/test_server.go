package main

import (
	"encoding/binary"
	"fmt"
	"net"
)

type foo struct {
	Foo int32
	Bar int32
	Baz int32
}

func main() {
	l, err := net.Listen("tcp", ":9090")
	if err != nil {
		panic(err)
	}
	defer l.Close()

	for {
		c, err := l.Accept()
		if err != nil {
			fmt.Println("accepting:", err)
			continue
		}

		handle(c)
	}
}

func handle(c net.Conn) {
	defer c.Close()

	fmt.Printf("new conn: from %s, on %s\n", c.RemoteAddr(), c.LocalAddr())

	var f foo
	fmt.Print("reading...")
	if err := binary.Read(c, binary.LittleEndian, &f); err != nil {
		fmt.Print("error:", err, "...")
	}
	fmt.Println("done")

	fmt.Println("read:", f)

	f.Foo++
	f.Bar++
	f.Baz++

	fmt.Print("writing...")
	if err := binary.Write(c, binary.LittleEndian, f); err != nil {
		fmt.Print("error:", err, "...")
	}
	fmt.Println("done")

	fmt.Println("sent:", f)
}

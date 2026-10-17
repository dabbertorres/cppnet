package main

import (
	"errors"
	"fmt"
	"log"
	"net"
	"net/http"
)

func main() {
	srv := &http.Server{
		Addr: ":8080",
		Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			log.Println("handling request from", r.RemoteAddr)
			fmt.Fprintln(w, "Hello, World!")
			log.Println("response sent")
		}),
		ErrorLog: log.Default(),
		ConnState: func(c net.Conn, cs http.ConnState) {
			log.Printf("%s %s\n", c.RemoteAddr(), cs)
		},
	}

	log.Println("serving...")

	if err := srv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
		log.Fatal(err)
	}
}

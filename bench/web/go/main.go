// The TechEmpower plaintext and JSON tests with Go's standard library, for bench/web/run.sh.
package main

import (
	"encoding/json"
	"log"
	"net/http"
	"os"
)

type message struct {
	Message string `json:"message"`
}

var helloWorld = []byte("Hello, World!")

func main() {
	port := os.Getenv("PORT")
	if port == "" {
		port = "8080"
	}

	mux := http.NewServeMux()
	mux.HandleFunc("GET /plaintext", func(w http.ResponseWriter, r *http.Request) {
		h := w.Header()
		h.Set("Server", "Go")
		h.Set("Content-Type", "text/plain")
		w.Write(helloWorld)
	})
	mux.HandleFunc("GET /json", func(w http.ResponseWriter, r *http.Request) {
		body, err := json.Marshal(&message{Message: "Hello, World!"})
		if err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		h := w.Header()
		h.Set("Server", "Go")
		h.Set("Content-Type", "application/json")
		w.Write(body)
	})

	log.Fatal(http.ListenAndServe(":"+port, mux))
}

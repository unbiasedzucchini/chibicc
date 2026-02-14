package main

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"
	"os/exec"
)

const (
	chibiccBin  = "/home/exedev/chibicc/chibicc"
	staticDir   = "/home/exedev/chibicc/explorer"
	listenAddr  = ":8001"
	cmdTimeout  = 5 * time.Second
)

// --- Request / Response types ---

type CompileRequest struct {
	Code string `json:"code"`
}

type StageStats struct {
	// tokenize
	Count int `json:"count,omitempty"`
	// preprocess
	Lines int `json:"lines,omitempty"`
	// parse
	Functions int `json:"functions,omitempty"`
	Globals   int `json:"globals,omitempty"`
	Nodes     int `json:"nodes,omitempty"`
	// codegen
	Bytes int `json:"bytes,omitempty"`
	// common
	TimeMs float64 `json:"time_ms"`
}

type CompileResponse struct {
	Tokens       json.RawMessage    `json:"tokens"`
	Preprocessed string             `json:"preprocessed"`
	AST          json.RawMessage    `json:"ast"`
	Assembly     string             `json:"assembly"`
	Error        *string            `json:"error"`
	Stages       map[string]*StageStats `json:"stages"`
}

// --- Helpers ---

// runCmd executes a command with a timeout and returns stdout, stderr, error.
func runCmd(ctx context.Context, name string, args ...string) (string, string, error) {
	cmd := exec.CommandContext(ctx, name, args...)
	var stdout, stderr strings.Builder
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	err := cmd.Run()
	return stdout.String(), stderr.String(), err
}

// countASTNodes recursively counts objects with a "kind" field.
func countASTNodes(v interface{}) int {
	count := 0
	switch val := v.(type) {
	case map[string]interface{}:
		if _, ok := val["kind"]; ok {
			count++
		}
		for _, child := range val {
			count += countASTNodes(child)
		}
	case []interface{}:
		for _, item := range val {
			count += countASTNodes(item)
		}
	}
	return count
}

// countFunctionsAndGlobals counts top-level entries in the AST.
func countFunctionsAndGlobals(astObj map[string]interface{}) (functions, globals int) {
	globalsList, ok := astObj["globals"]
	if !ok {
		return
	}
	arr, ok := globalsList.([]interface{})
	if !ok {
		return
	}
	for _, item := range arr {
		obj, ok := item.(map[string]interface{})
		if !ok {
			continue
		}
		isFunc, _ := obj["is_function"].(bool)
		if isFunc {
			functions++
		} else {
			globals++
		}
	}
	return
}

// --- Compile handler ---

func handleCompile(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var req CompileRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "invalid JSON: "+err.Error(), http.StatusBadRequest)
		return
	}
	if req.Code == "" {
		http.Error(w, "empty code", http.StatusBadRequest)
		return
	}

	// Create temp directory for this compilation.
	tmpDir, err := os.MkdirTemp("", "chibicc-explorer-*")
	if err != nil {
		http.Error(w, "failed to create temp dir", http.StatusInternalServerError)
		return
	}
	defer os.RemoveAll(tmpDir)

	srcFile := filepath.Join(tmpDir, "input.c")
	if err := os.WriteFile(srcFile, []byte(req.Code), 0644); err != nil {
		http.Error(w, "failed to write temp file", http.StatusInternalServerError)
		return
	}

	resp := CompileResponse{
		Tokens: json.RawMessage("null"),
		AST:    json.RawMessage("null"),
		Stages: make(map[string]*StageStats),
	}

	var errors []string

	// Stage 1: Tokenize (--dump-tokens)
	{
		ctx, cancel := context.WithTimeout(context.Background(), cmdTimeout)
		defer cancel()
		start := time.Now()
		stdout, stderr, err := runCmd(ctx, chibiccBin,
			"--dump-tokens", "-cc1", "-cc1-input", srcFile, "-cc1-output", "/dev/null", srcFile)
		elapsed := time.Since(start)

		stats := &StageStats{TimeMs: float64(elapsed.Microseconds()) / 1000.0}
		if err != nil {
			msg := fmt.Sprintf("tokenize: %s", strings.TrimSpace(stderr))
			if msg == "tokenize: " {
				msg = fmt.Sprintf("tokenize: %v", err)
			}
			errors = append(errors, msg)
		} else {
			// stdout has the JSON token array
			var tokens []interface{}
			if jsonErr := json.Unmarshal([]byte(stdout), &tokens); jsonErr == nil {
				stats.Count = len(tokens)
				resp.Tokens = json.RawMessage(stdout)
			} else {
				// Maybe it's valid JSON but not an array; store raw
				resp.Tokens = json.RawMessage(stdout)
			}
		}
		resp.Stages["tokenize"] = stats
	}

	// Stage 2: Preprocess (-E)
	{
		ctx, cancel := context.WithTimeout(context.Background(), cmdTimeout)
		defer cancel()
		start := time.Now()
		stdout, stderr, err := runCmd(ctx, chibiccBin,
			"-E", "-cc1", "-cc1-input", srcFile, "-cc1-output", "/dev/stdout", srcFile)
		elapsed := time.Since(start)

		stats := &StageStats{TimeMs: float64(elapsed.Microseconds()) / 1000.0}
		if err != nil {
			msg := fmt.Sprintf("preprocess: %s", strings.TrimSpace(stderr))
			if msg == "preprocess: " {
				msg = fmt.Sprintf("preprocess: %v", err)
			}
			errors = append(errors, msg)
		} else {
			resp.Preprocessed = stdout
			stats.Lines = len(strings.Split(strings.TrimRight(stdout, "\n"), "\n"))
		}
		resp.Stages["preprocess"] = stats
	}

	// Stage 3: Parse / AST (--dump-ast)
	{
		ctx, cancel := context.WithTimeout(context.Background(), cmdTimeout)
		defer cancel()
		start := time.Now()
		stdout, stderr, err := runCmd(ctx, chibiccBin,
			"--dump-ast", "-cc1", "-cc1-input", srcFile, "-cc1-output", "/dev/null", srcFile)
		elapsed := time.Since(start)

		stats := &StageStats{TimeMs: float64(elapsed.Microseconds()) / 1000.0}
		if err != nil {
			msg := fmt.Sprintf("parse: %s", strings.TrimSpace(stderr))
			if msg == "parse: " {
				msg = fmt.Sprintf("parse: %v", err)
			}
			errors = append(errors, msg)
		} else {
			var astObj map[string]interface{}
			if jsonErr := json.Unmarshal([]byte(stdout), &astObj); jsonErr == nil {
				functions, globals := countFunctionsAndGlobals(astObj)
				stats.Functions = functions
				stats.Globals = globals
				stats.Nodes = countASTNodes(astObj)
				resp.AST = json.RawMessage(stdout)
			} else {
				// Store raw even if we can't parse
				resp.AST = json.RawMessage(stdout)
			}
		}
		resp.Stages["parse"] = stats
	}

	// Stage 4: Codegen (-S)
	{
		asmFile := filepath.Join(tmpDir, "output.s")
		ctx, cancel := context.WithTimeout(context.Background(), cmdTimeout)
		defer cancel()
		start := time.Now()
		_, stderr, err := runCmd(ctx, chibiccBin,
			"-S", "-o", asmFile, srcFile)
		elapsed := time.Since(start)

		stats := &StageStats{TimeMs: float64(elapsed.Microseconds()) / 1000.0}
		if err != nil {
			msg := fmt.Sprintf("codegen: %s", strings.TrimSpace(stderr))
			if msg == "codegen: " {
				msg = fmt.Sprintf("codegen: %v", err)
			}
			errors = append(errors, msg)
		} else {
			asmBytes, readErr := os.ReadFile(asmFile)
			if readErr == nil {
				resp.Assembly = string(asmBytes)
				stats.Lines = len(strings.Split(strings.TrimRight(string(asmBytes), "\n"), "\n"))
				stats.Bytes = len(asmBytes)
			} else {
				errors = append(errors, fmt.Sprintf("codegen: failed to read output: %v", readErr))
			}
		}
		resp.Stages["codegen"] = stats
	}

	// Combine errors
	if len(errors) > 0 {
		combined := strings.Join(errors, "\n")
		resp.Error = &combined
	}

	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Access-Control-Allow-Origin", "*")
	json.NewEncoder(w).Encode(resp)
}

func main() {
	// Verify chibicc binary exists
	if _, err := os.Stat(chibiccBin); os.IsNotExist(err) {
		log.Fatalf("chibicc binary not found at %s", chibiccBin)
	}

	// API endpoint
	http.HandleFunc("/api/compile", handleCompile)

	// Static file server (for index.html, etc.)
	fs := http.FileServer(http.Dir(staticDir))
	http.Handle("/", fs)

	log.Printf("chibicc explorer server starting on http://localhost%s", listenAddr)
	log.Printf("  chibicc binary: %s", chibiccBin)
	log.Printf("  static files:   %s", staticDir)
	if err := http.ListenAndServe(listenAddr, nil); err != nil {
		log.Fatal(err)
	}
}

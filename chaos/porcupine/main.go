// Checks a chaos history for linearizability against a sequencer model with Porcupine: every acked
// command gets a log index, and a command acked before another was invoked must have the lower
// index. Commands that never got an answer may or may not have happened.
package main

import (
	"bufio"
	"fmt"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/anishathalye/porcupine"
)

type output struct {
	known bool
	index uint64
}

var sequencer = porcupine.Model{
	Init: func() interface{} { return uint64(0) },
	Step: func(state, input, out interface{}) (bool, interface{}) {
		o := out.(output)
		last := state.(uint64)
		if !o.known {
			return true, last
		}
		// Commands batched into one entry share its index.
		return o.index >= last, o.index
	},
	Equal: func(a, b interface{}) bool { return a == b },
	DescribeOperation: func(input, out interface{}) string {
		o := out.(output)
		if !o.known {
			return fmt.Sprintf("%v -> ?", input)
		}
		return fmt.Sprintf("%v -> %d", input, o.index)
	},
}

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: porcupine history.tsv")
		os.Exit(2)
	}
	file, err := os.Open(os.Args[1])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	defer file.Close()
	var ops []porcupine.Operation
	var end int64
	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		f := strings.Split(scanner.Text(), "\t")
		if len(f) != 8 {
			continue
		}
		worker, _ := strconv.Atoi(f[0])
		call, _ := strconv.ParseInt(f[3], 10, 64)
		ret, _ := strconv.ParseInt(f[4], 10, 64)
		index, _ := strconv.ParseUint(f[6], 10, 64)
		if ret > end {
			end = ret
		}
		op := porcupine.Operation{ClientId: worker, Input: f[1] + "/" + f[2], Call: call, Return: ret}
		op.Output = output{known: f[5] == "ok", index: index}
		ops = append(ops, op)
	}
	// An unanswered command may take effect any time after it was sent.
	for i := range ops {
		if !ops[i].Output.(output).known {
			ops[i].Return = end + 1
		}
	}
	result, _ := porcupine.CheckOperationsVerbose(sequencer, ops, 5*time.Minute)
	fmt.Printf("operations=%d result=%v\n", len(ops), result)
	if result != porcupine.Ok {
		os.Exit(1)
	}
}

package main

import (
	"encoding/json"
	"os"
	"regexp"
	"strings"
)

func main() {
	var input struct {
		Patterns map[string]string
		Commands []string
	}
	if err := json.NewDecoder(os.Stdin).Decode(&input); err != nil {
		panic(err)
	}
	patterns := make(map[string]*regexp.Regexp)
	for name, pattern := range input.Patterns {
		patterns[name] = regexp.MustCompile(pattern)
	}
	output := make([]bool, len(input.Commands))
	for i, item := range input.Commands {
		command := item[2:]
		kind := "hostRole"
		if item[:2] == "b:" {
			kind = "hostAdmin"
			if strings.HasPrefix(command, "role config ") || strings.HasPrefix(command, "role name ") {
				output[i] = command == "role config bot" || command == "role name bot"
				continue
			}
		}
		output[i] = len(command) <= 160 &&
			(patterns[kind+"Read"].MatchString(command) || patterns[kind+"Write"].MatchString(command))
	}
	if err := json.NewEncoder(os.Stdout).Encode(output); err != nil {
		panic(err)
	}
}

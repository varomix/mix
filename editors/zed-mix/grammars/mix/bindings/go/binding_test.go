package tree_sitter_mix_test

import (
	"testing"

	tree_sitter "github.com/smacker/go-tree-sitter"
	"github.com/tree-sitter/tree-sitter-mix"
)

func TestCanLoadGrammar(t *testing.T) {
	language := tree_sitter.NewLanguage(tree_sitter_mix.Language())
	if language == nil {
		t.Errorf("Error loading Mix grammar")
	}
}

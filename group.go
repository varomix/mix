package mix

import (
	sf "github.com/manyminds/gosfml"
)

type Group struct {
	grp *sf.Drawer
}

func NewGroup() *Group {

	return &Group{nil}

}

package mix

import sf "github.com/manyminds/gosfml"

//type Linear struct {
//	tim    float32
//	begin  float32
//	change float32
//	dur    float32
//}

type Tween struct {
	pos *sf.Vector2f
}

func Linear(t, b, c, d float32) float32 {
	return c*(t/d) + b
}

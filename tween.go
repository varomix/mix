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

type vector struct {
	*sf.Vector2f
}

func Linear(b, c, d float32) float32 {
	//t := time.NewTicker(time.Second * 2).C

	//return c*(float32(t)/d) + b
	return 0
}

func (v vector) Sub(a, b vector) vector {
	v.X = a.X - b.X
	v.Y = a.Y - b.Y

	return v

}

func Mover(b *Button, x, y, speed float32, dir string) {
	oldx := b.GetPos().X - 128
	switch dir {
	case "left":
		if oldx-b.GetPos().X < 0 {
			b.Move(-speed, 0)
		}
	case "right":
		if x-b.GetPos().X > 0 {
			b.Move(speed, 0)

		}

	}
}

package mix

import "fmt"

// Button created
type button struct {
	X    int
	Y    int
	Text string
}

// NewBtn is created
func NewBtn(x, y int, text string) *button {
	b := new(button)
	b.X = x
	b.Y = y
	b.Text = text
	return b
}

// NewButton create the button
func (b *button) NewButton() {
	fmt.Printf("Button X: %d, Y: %d and the message is: %s", b.X, b.Y, b.Text)

}

/*
// OnClick the button action to trigger
func (b *Button) OnClick() {
	fmt.Println("You clicked the button")
}

// DrawButton drawing the button
func (b *Button) DrawButton() {
	fmt.Println("drawing button")
	fmt.Println("drawing button")
}
*/

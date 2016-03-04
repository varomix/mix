package mix

import sf "github.com/manyminds/gosfml"

// MixButton created
// type MixButton struct {
// 	X    int
// 	Y    int
// 	Text string
// }

// Button create the button
type Button struct {
	rect *sf.RectangleShape
	text *sf.Text
	font *sf.Font
}

// NewButton create the button
func NewButton() *Button {
	// Load font
	font, _ := sf.NewFontFromFile("assets/fonts/LuckiestGuy.ttf")

	// Title
	text, _ := sf.NewText(font)
	text.SetString("play")
	text.SetCharacterSize(36)

	rect, _ := sf.NewRectangleShape()
	rect.SetSize(sf.Vector2f{128, 32})
	rect.SetFillColor(sf.ColorRed())

	shape := &Button{rect, text, font}
	return shape
}

// SetText set the button text
func (b *Button) SetText(txt string) {
	b.text.SetString(txt)
}

// SetText set the button text
func (b *Button) Move(x, y float32) {
	b.rect.Move(sf.Vector2f{x, y})
	b.text.Move(sf.Vector2f{x, y})
}

// Draw the button on the screen
func (b *Button) Draw(target sf.RenderTarget, renderStates sf.RenderStates) {
	b.rect.Draw(target, renderStates)
	b.text.Draw(target, renderStates)

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

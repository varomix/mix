package mix

import (
	"fmt"

	sf "github.com/manyminds/gosfml"
)

// Button create the button
type Button struct {
	rect *sf.RectangleShape
	text *sf.Text
	font *sf.Font
}

// NewButton create the button
func NewButton() *Button {
	// Load font
	font, _ := sf.NewFontFromFile("assets/fonts/Homenaje.ttf")

	// Title
	text, _ := sf.NewText(font)
	text.SetString("play")
	text.SetCharacterSize(20)
	text.SetColor(sf.Color{180, 180, 180, 255})

	// TODO put colors on a struct

	rect, _ := sf.NewRectangleShape()
	rect.SetSize(sf.Vector2f{128, 128})
	rect.SetFillColor(sf.Color{30, 30, 30, 255})
	rect.SetOutlineThickness(-2)
	rect.SetOutlineColor(sf.ColorBlack())
	rect.SetOrigin(sf.Vector2f{rect.GetSize().X / 2, rect.GetSize().Y / 2})

	text.SetOrigin(sf.Vector2f{text.GetLocalBounds().Left + text.GetLocalBounds().Width/2, text.GetLocalBounds().Top + text.GetLocalBounds().Height/2})
	text.SetPosition(sf.Vector2f{rect.GetLocalBounds().Width / 2, rect.GetGlobalBounds().Height / 2})

	shape := &Button{rect, text, font}
	return shape
}

// SetText set the button text
func (b *Button) SetText(txt string) {
	b.text.SetString(txt)
	b.alignTextCenter()
}

// SetText set the button text
func (b *Button) SetSize(width, height float32) {
	b.rect.SetSize(sf.Vector2f{width, height})
	b.rect.SetOrigin(sf.Vector2f{b.rect.GetSize().X / 2, b.rect.GetSize().Y / 2})
	b.alignTextCenter()
}

// SetText set the button text
func (b *Button) Move(x, y float32) {
	//b.alignTextCenter()
	b.rect.Move(sf.Vector2f{x, y})
	b.text.Move(sf.Vector2f{x, y})

}

// SetText set the button text
func (b *Button) alignTextCenter() {
	b.text.SetOrigin(sf.Vector2f{b.text.GetLocalBounds().Left + b.text.GetLocalBounds().Width/2, b.text.GetLocalBounds().Top + b.text.GetLocalBounds().Height/2})
	b.text.SetPosition(sf.Vector2f{b.rect.GetPosition().X, b.rect.GetPosition().Y})

}

// Events for the button
func (b *Button) Events(event sf.Event) {
	body := sf.FloatRect{b.rect.GetPosition().X - b.rect.GetLocalBounds().Width/2, b.rect.GetPosition().Y - b.rect.GetLocalBounds().Height/2, b.rect.GetSize().X, b.rect.GetSize().Y}

	// check if the button is pressed and the mouse is on top of the button
	switch ev := event.(type) {
	case sf.EventMouseButtonPressed:
		if ev.Button == 0 && body.Contains(float32(ev.X), float32(ev.Y)) {
			b.onPressed()
		}

	case sf.EventMouseButtonReleased:
		if ev.Button == 0 && body.Contains(float32(ev.X), float32(ev.Y)) {
			b.onClick()
		}

	case sf.EventMouseMoved:
		if body.Contains(float32(ev.X), float32(ev.Y)) {
			fmt.Println("Mouse is over")
			b.rect.SetFillColor(sf.Color{120, 120, 120, 255})
			b.text.SetColor(sf.ColorBlack())
		} else {
			b.rect.SetFillColor(sf.Color{30, 30, 30, 255})
			b.text.SetColor(sf.Color{180, 180, 180, 255})
		}

	}
}

func (b *Button) onPressed() {
	b.rect.SetFillColor(sf.ColorWhite())
}

func (b *Button) onClick() {
	b.rect.SetFillColor(sf.Color{120, 120, 120, 255})
	b.text.SetColor(sf.ColorBlack())
	fmt.Printf("The button was clicked, it's location is X: %v, Y: %v\n", b.rect.GetPosition().X, b.rect.GetPosition().Y)
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

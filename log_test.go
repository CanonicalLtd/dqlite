package dqlite_test

import (
	"bytes"
	"testing"

	"github.com/CanonicalLtd/dqlite"
)

func TestLevelFilterWithOrigin_Write(t *testing.T) {
	writer := bytes.NewBuffer(nil)

	cases := []struct {
		origins []string
		message string
		written bool
	}{
		{[]string{"foo"}, "[INFO] foo: hello", true},
		{[]string{"foo"}, "[DEBUG] foo: hello", false},
		{[]string{"foo"}, "[INFO] bar: hello", false},
		{[]string{"foo"}, "foo: hello", true},
		{[]string{"foo"}, "hello", true},
		{nil, "[INFO] bar: hello", true},
	}

	for _, c := range cases {
		t.Run(c.message, func(t *testing.T) {
			defer writer.Reset()
			filter := dqlite.NewLogFilter(writer, "", c.origins)

			filter.Write([]byte(c.message))

			want := ""
			if c.written {
				want = c.message
			}
			if got := writer.String(); got != want {
				t.Errorf("got %#v, wanted %#v", got, want)
			}
		})
	}
}

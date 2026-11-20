package main

import "testing"

func TestTrimSessionKeepsSystemAndUserBoundary(t *testing.T) {
	sys := "sys"
	s := &session{messages: []oaiMessage{{Role: "system", Content: &sys}}}
	for i := 0; i < 100; i++ {
		role := "user"
		if i%2 == 1 {
			role = "assistant"
		}
		c := "m"
		s.messages = append(s.messages, oaiMessage{Role: role, Content: &c})
	}
	trimSession(s)
	if len(s.messages) > 60 {
		t.Fatalf("too many messages after trim: %d", len(s.messages))
	}
	if s.messages[0].Role != "system" {
		t.Fatalf("system message lost")
	}
	if len(s.messages) > 1 && s.messages[1].Role != "user" {
		t.Fatalf("cut point is not a user message: %s", s.messages[1].Role)
	}
}

func TestTrimSessionNoUserBoundary(t *testing.T) {
	sys := "sys"
	s := &session{messages: []oaiMessage{{Role: "system", Content: &sys}}}
	for i := 0; i < 100; i++ {
		c := "m"
		s.messages = append(s.messages, oaiMessage{Role: "assistant", Content: &c})
	}
	before := len(s.messages)
	trimSession(s)
	if len(s.messages) != before {
		t.Fatalf("should not trim without a user boundary")
	}
}

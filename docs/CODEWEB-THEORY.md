# Codeweb theory

*A way of looking at programs and the systems they live in. By ExCorvus, 2026.*

## The short version

A program is never alone. It lives inside a system: the operating system, the hardware, the libraries it calls.
Most people picture this as layers, like a cake: the program on top, the system underneath, each on its own.

Codeweb theory says: **it is not a cake, it is a fabric.** The program and its system are woven into each other,
thread by thread, so tightly that they are really one piece of software. If you want to move a program to a
different machine, you cannot just put a new plate under the cake. You have to weave the new machine into the
fabric, thread for thread, so that every thread finds the partner it was woven with.

The way of doing that is called **weaving**.

## The picture: a hermit crab and its shell

A hermit crab lives in a shell that was not made for it, but it fits the crab's body exactly. If you want the
crab to move into a new home, you do not reshape the crab. You build a new shell with the same inside: the same
curves, the same depth, the same spot where the crab holds on.

A program that was made for a game console is the crab. The console was its shell. To bring the program to a PC,
you do not change the program. You build a shell on the PC that has **the same inside as the one it expects**.
The program should not be able to tell that it moved.

## Why "web" and not "shell"

At first it looks as if the shell is just around the program. But when you measure closely, you see that the
program reaches out to its system thousands of times: for memory, for time, for pictures, for sound. And the
system reaches back: it answers, it reports events, it calls the program at the right moments.

That is a web, not a shell. Pull one thread, and the whole web moves. That has three consequences:

1. **Everything is connected.** Memory, timing, the picture and the sound are not separate problems. They are
   one system that has to be in balance.
2. **The system answers back.** It is a conversation, not an order. If you put a fake answer where the real
   system would have answered, the mistake sits inside the fabric, and it shows up somewhere else.
3. **Use what is already there.** Where the new machine already has the same thing (a clock, a window, a
   sound device), the program should talk to that thing directly. The shell only adds what is really missing.

## The same answer in another language

Sometimes the new machine cannot give exactly the same answer, because it speaks a different "language". Then
the answer must still be **correct**, just said differently.

Think of an interpreter at a meeting. She does not invent an answer, and she does not stay silent. She says the
same thing, correctly, in the other language. The question is never left unanswered, and the work is really done.

## Rules that follow from it

**Errors point away from their cause.** In medicine there is "referred pain": a heart problem can be felt in
the left arm. In a woven program it is the same. Where it crashes is rarely where the problem is. So you do not
treat the arm. You follow the thread back.

**Let a failure stand (negative verification).** If a thermometer shows a fever, you do not fix the thermometer.
A failure is evidence. You write down what failed and where, and only then decide. If you "fix it into working"
first, you destroy the evidence.

**Untangle before you build.** A knotted necklace is not repaired by cutting it and tying on new pieces. You
untangle it. First understand how the threads run; only then add a thread of your own.

**Measure, do not assume.** Every answer is read from the program itself. Before building a road, you look at
the map.

**The same ending can hide progress.** A runner who always stumbles at the same hurdle may still be getting
faster every time. If you only look at where she falls, you miss how far she has come. So you also measure the
whole run.

**Words steer.** In the children's game of "telephone", a message whispered from ear to ear ends up as
something else. When work is summarised again and again (by people, or by AI assistants whose memory is
compressed), a small slip in wording becomes the direction everyone follows. So the words have to be as exact
as the measurements.

## Working with AI assistants

This theory grew out of a project done together with AI coding assistants. One pattern showed up again and
again: an assistant that sees an error tends to "fix" it, so that the error disappears, even when the fix only
hides the real cause. That is the thermometer being repaired instead of the fever being treated.

The rules above are also a protection against that: let failures stand, follow the thread back, keep the words
exact, and only accept a step when the whole program runs, not just one piece.

## What it builds on

Codeweb theory stands on the shoulders of others: programs like Wine and Proton, which answer the calls a
program makes instead of imitating hardware; research on moving device software to new environments; and
classic debugging knowledge that errors travel before they show. What Codeweb adds is the view that program,
shell and system are **one fabric**, and the working rules that come from taking that view seriously.

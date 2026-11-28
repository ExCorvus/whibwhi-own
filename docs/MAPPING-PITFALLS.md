# Pitfalls when mapping a program

A short list from this project's own measurements (each one is in [FINDINGS.md](../FINDINGS.md)). It is meant
for anyone who maps what a program asks of the system it was made for, and answers it on another.

## Things that look healthy but are not

1. **A run that gets further is not progress.** Answering a call with "success" for work that was not done lets
   the program go on and fail later, somewhere else. The run looks better; the cause is hidden.
2. **"Not implemented" can look harmless.** The program may have a second way and take it quietly. Everything
   still runs, but its main way never works: here the program's ready-made base pipelines were refused, so it
   built every pipeline the long way instead.
3. **The same answer to every request.** Tens of thousands of different requests all got one identical refusal.
   The counts looked normal; only comparing the answers showed it.
4. **A log that ends early.** A trace that stopped before the files were opened read as "the files are missing".
   They were not.
5. **Your own measuring rhythm.** Progress that seemed to come "every half second" was the tool's own sampling
   interval, not the program's.
6. **A stale file in the test folder.** One library stayed an older copy, and the run used it. Check that the run
   uses exactly the build you think it does.
7. **Counting per thread hides what other threads do.** A request with "no fallback" had one: on another thread,
   later.
8. **Two tools that disagree.** One script reported the window as resized; another measured it at its old size for
   the whole run. When two measurements disagree, the disagreement is the finding.
9. **A description is not a limit.** The tables a program declares said "two places"; its shaders read further, as
   the hardware it was made for allows. Copying the declared size refused correct work.

## Things that block you from behind

1. **Time that falls on the program's own clock.** Work done while the program waits (translating shaders,
   building pipelines) counts against its watchdog and frame pacing. Do it before, and keep it.
2. **Your own test procedure.** Nine minutes of re-measuring an unchanged file before every run. Measure only
   when something changed.
3. **The right library in the wrong version.** The system's own file of the expected name was an old one; the
   current one was installed under another name.
4. **One refusal hides the next.** Closing one gap shows the next stop behind it. Count what is still refused
   after every step, not only what got through.
5. **An answer of your own where the system has one.** If your code answers in place of the operating system
   (display, timing, input), the error sits inside the program's world, where it is hardest to see.
6. **The crash site is a symptom.** The program fell over reading an empty value. The question is who should have
   filled it, not how to make the read survive.
7. **A switch you did not see.** A path looked closed by a version check; it was a single flag byte.
8. **A fix that erases the evidence.** Making a failure pass before writing down what failed and where removes the
   one clue to the cause. Let it fail, write it down, then decide.

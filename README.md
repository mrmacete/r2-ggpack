# r2-ggpack

Suite of r2 plugins to read and manipulate the **ggpack** data files used by Thimbleweed Park game.

## Disclaimer

This is a totally unofficial, unwanted and unrequested work, based on reverse engineering.

Expect a lot of bugs, if you're brave enough to open your data files in write mode with this plugin **be sure to work on a copy** and **make a full game backup first**.

## Show / Cinema

TODO

## Thanks

This wouldn't have been possible without the hints contained in the awesome [https://github.com/mstr-/twp-ggdump](https://github.com/mstr-/twp-ggdump) (even if it turned out that abstraction was wrong)

## Why

- because it's fun
- to allow more reversing of the game (in the hope to ease the burden of re-writing its engine in the open sources in the future)
- to enable full mods

## What you can do

- read the contents of the ggpack files
	- browse all files into the ggpack
	- search for content
	- edit them in-place using r2
- dump the content to files
- edit the unpacked files with external editors (GIMP, etc)
- import them again into the ggpack, so that you can just plug that back in the game and see your modifications

## How does it work

- there's an **RIO** plugin which does most of the job:
	- on-the-fly deobfuscation / obfuscation
	- index directory management

- a shallow **RBin** plugin just puts a flag for each packed file
- a dumb python test suite exists to ensure a minimum level of internal coherence

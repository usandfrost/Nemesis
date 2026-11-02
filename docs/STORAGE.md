# The SD card

What the firmware is built to do with a card, and what has and has not been shown.

**Disclaimer:** Nemesis is free software, given as it is, with no warranty of any kind. You use it at your own risk. Keep a copy of your SD card: a power cut during a save, or a failing card, can lose data, and installing other firmware on your device is your own choice. The makers accept no liability for lost data or for damage to cards or devices.

## The aim

On a healthy card: **a save either completes, or the previous version stays, and the card stays readable on a computer.**

That is what the design aims at and what the tests below check. It is not a guarantee, and it does not mean a card can never be corrupted. The SD specification gives no protection to data being written when the power goes, and a card can simply fail. A sector of a folder or of the FAT holds many files' records, so if a cut destroys a whole sector of those, or the flash page around it, no scheme of files on FAT can save what it held. What the firmware does is make sure it never makes things worse itself.

## The rules

**Starting up writes nothing.** The firmware mounts the card, lists what is on it and reads what it needs. A card that is only ever played from stays exactly as it came. The folders it needs are made at the first save. The preset index is rebuilt in memory and goes to the card with the next save of a project or a preset. The one exception: a preset replace that a power cut stopped half way is finished at the next start, after the check below. A card that has never been saved to has an empty library.

**The whole file system is checked before the first write after a start** (`firmware/src/storage/CardCheck.cpp`). FatFs believes the FAT: a cluster marked free is free, whatever a folder says. So a damaged card gets more damaged by healthy firmware. The check reads every FAT entry, every folder and every chain, shares no code with FatFs, and writes nothing.

- Refused: a FAT entry outside the volume, a file running through a free cluster, two files sharing a cluster, a file cut short, a backup boot sector that describes a different volume, a volume bigger than the card, the hard-error flag.
- Also refused, because the check only reads FAT32: FAT12 and FAT16, which is how cards of 2 GB and less can come. They still play, and saves ask for FAT32.
- Allowed, because they are harmless and a power cut can leave them: lost clusters, a chain longer than its file, a second FAT behind the first, the clean-shutdown flag being off.

One thing is cleared rather than refused: old data after a folder's end mark. Readers never see it, but a new file overwrites the mark and it would come to life as entries, so it is zeroed before the first write. Up to 16 clusters of it; more than that is refused.

A refused card keeps playing. Saves fail with the reason on the screen, and it needs a disk check on a computer. **The firmware never repairs or formats anything.**

**A fault stops all writing until the next start** (`sd_diskio.c`). A failed or timed-out transfer is aborted so it cannot land late. A write the card turned down, which only shows in the status after it, counts as failed. FatFs is told the card is write-protected, and it is mounted again so it holds nothing it cannot write. Reading goes on.

**Every save is read back from the card before it counts.**

**Projects** are two files taken in turn, `PROJn.DBP` and `PROJn.BAK`. A save first reads the newest file all the way through, then overwrites the older one (or the newest, if it turns out damaged), and a mark at the end of the file counts the saves. The newest whole file is the project and the one before it is what SHIFT + ○ goes back to, so the newest may be either name. Undo takes back only the save made in this session, and a load says so if it had to fall back on the older file. Nothing is ever renamed: FatFs's rename is two directory writes, and a power cut between them leaves two names on one chain, which is exactly what the check refuses.

Deleting a project removes both files, the older one first: a power cut between the two leaves the project as it was, and never the save before it in its place. There is no copy on the card, because keeping one would mean a rename. The undo is a copy in memory, made from what a load would give, so a sample no longer on the card stays lost: the message says so at the delete. It is gone with the power or the next undoable action, and it costs no memory of its own: it shares the buffer a load's undo uses. A project that cannot be read takes one press more and has no undo.

**Presets** are replaced where they lie. The old bytes go to `CATHODE/PRESET.JNL`, are read back, and stay until the new ones have been read back. The next start keeps whichever of the two is whole.

**The D-cache.** No line of a buffer stays in the cache while the DMA works on it, and a buffer is invalidated after a read too, because the core fetches ahead. See the comment at the head of the driver's functions. This is the rule a card lost a folder to: one dirty line of the scratch buffer, evicted on top of a FAT sector the DMA had just delivered, written back by FatFs. Three clusters in use looked free, and a recording then landed on the folder.

**FatFs** is R0.16 with patches 1 and 2 and one change of ours, marked in `ff.c`: a folder's new cluster is cleared before the FAT that links it is written. Upstream does it the other way round, and a cut between the two leaves a folder running into old data. It reads no FSInfo: the free space comes from the FAT. Nothing that can format or relabel a card is compiled in.

## What has been shown

`tests/storage` (in `tools/check_all.sh`) runs the real `sd_diskio.c`, `bsp_driver_sd.c`, FatFs and `Card.cpp` on a modelled card, DMA and cache, and an independent reader of the image judges the result.

- The cache rules: dirty lines evicted during transfers, lines fetched ahead. Each rule is also taken away, and that run has to fail. The old driver, run by hand on the same workload, fails every time.
- Failed, hung, rejected and late transfers: reported, and nothing is written after one.
- A start writes nothing, byte for byte.
- A power cut across each kind of save, with the sector at the cut torn at a random byte, or the last acknowledged sector lost, and a second cut during the repair: no save lost, and the card goes on saving. A naive save goes through the same sweep and has to lose projects, so the sweep is known to see them.
- A wider attack on an earlier state of this code, 1.2 million cuts with every sector, 22 ways of tearing and second cuts: with the sector at the cut landing or not, and up to 8 acknowledged sectors lost, nothing was lost. It found the folder fault above. A tear inside a sector can cost a refusal: the card then needs a computer's disk check, though no save was lost. Tearing a whole sector or flash page of a folder or the FAT does lose saves, as the promise above says.
- The check refuses the real damaged card, passes a card macOS formatted and filled, and still refuses the damaged card with its bad entries made to look valid.

## What has been shown on the board

`soak ROUNDS` in `tools/board_script.py` runs `board/CardSoak` through the debug mailbox, with the sequencer playing so the cache stays under load. On one new 32 GB SanDisk card, FAT32 at 32 KB clusters, holding 2,996 files, on 2026-10-31:

| Rounds | File writes | File reads | Project saves | Preset saves | Full card checks | Data | Errors | Audio overruns | Worst block |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 300 | 300 | 514 | 37 | 18 | 4 | 39 MB | 0 | 0 | 42.7% |
| 3,000 | 3,000 | 5,468 | 375 | 187 | 46 | 348 MB | 0 | 0 | 46.5% |

The check before the first write after a start took 6 to 8 s on that card, with the main loop blocked and the sound playing on. It runs once a start. That is all one card, with steady power.

## What has not been shown

The power-cut results above are from the model. **Power cuts have not been tested on real hardware or real cards.** A rig with switched power across several card models was planned and dropped (2026-10-31): the firmware is free, and the disclaimer above covers what remains. So nobody knows how often a real card loses a folder or FAT sector in a cut, and a model's torn sector may not be what a real card does.

Also open:

- How long the check takes on a full or slower card, and whether a slow first save needs a message of its own.
- Whether a power-fail warning (PVD) can be read on this board.


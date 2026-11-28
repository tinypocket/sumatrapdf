# Releasing SumatraPDF+ ("bump")

The fork ships as GitHub releases on `tinypocket/sumatrapdf`, from the
`sumatraplus-ui-updates` branch. The app updates itself from them, so a release
is not finished until it is the repository's **Latest** release and carries a
matching `update-check.txt`.

When the user says **"bump"**, they mean this whole sequence: commit, raise the
version, build, package, smoke test, push, publish.

## 1. Commit the work

Never commit without being told to; "bump" is being told to.

The message ends with the prompts that produced the change, then the
attribution lines the session is configured with:

```
<subject: what changed, in the user's terms>

<body: why, and anything surprising about how>

prompt: <the user's substantive requests, squashed into one line>

Co-Authored-By: ...
Claude-Session: ...
```

Run clang-format on every `src/` file you touched before building (see
`agents.md`).

## 2. Raise the version

Two files, three places, all of which must agree:

- `src/Version.h` — `CURR_VERSION` (e.g. `3.7.33`) and `CURR_VERSION_COMMA`
  (e.g. `3, 7, 33`)
- `update-check.txt` — the `Latest:` line

The third component is the fork's own release number; bump it every time.
`CompareProgramVersion` treats a missing component as 0, so 3.7.33 > 3.7. If
the version does not change, the update check reports "You have the latest
version" and nobody gets the build.

Commit this on its own as `release <X.Y.Z>`.

## 3. Build and package

One command builds `SumatraPDF` and `SumatraPDF-static` for x64 and ARM64,
copies them into `out/packages/` under their release names, and writes
`SHA256SUMS.txt` (uppercase hashes, `HASH *name`):

```
bun cmd/package-sumatrapdf-plus.ts
```

Takes about four minutes when the tree is already built. It produces:

| file                                     | what it is                    |
| ---------------------------------------- | ----------------------------- |
| `SumatraPDF+-private-64-install.exe`     | x64 `SumatraPDF.exe`          |
| `SumatraPDF+-private-64-portable.exe`    | x64 `SumatraPDF-static.exe`   |
| `SumatraPDF+-private-arm64-install.exe`  | ARM64 `SumatraPDF.exe`        |
| `SumatraPDF+-private-arm64-portable.exe` | ARM64 `SumatraPDF-static.exe` |
| `SHA256SUMS.txt`                         | hashes of the four            |

Then copy the bumped `update-check.txt` next to them:

```
cp update-check.txt out/packages/update-check.txt
```

Kill leftover MSBuild workers afterwards (`taskkill /f /im MSBuild.exe`); they
hold onto the output directory.

## 4. Smoke test the packaged build

Run the packaged **portable** exe, not the debug build, and exercise whatever
the release changed:

```
SUMATRA_EXE="$PWD/out/packages/SumatraPDF+-private-64-portable.exe" bun tests/tmp/<test>.ts
```

The user's own debug instance holds a lock on `libsumatrapdf.dll`, which is why
testing always goes through a `-static` exe. Always pass `-for-testing`.

## 5. Push and publish

```
git push
gh release create v<X.Y.Z> -R tinypocket/sumatrapdf \
  --target sumatraplus-ui-updates \
  --title "SumatraPDF+ <X.Y.Z>" \
  --notes-file <notes.md> \
  "out/packages/SumatraPDF+-private-64-install.exe" \
  "out/packages/SumatraPDF+-private-64-portable.exe" \
  "out/packages/SumatraPDF+-private-arm64-install.exe" \
  "out/packages/SumatraPDF+-private-arm64-portable.exe" \
  out/packages/SHA256SUMS.txt \
  update-check.txt
```

Write the notes in the scratchpad, not the repo. They are for the person
installing the build: what changed and what it means for them, in their own
terms, not a changelog of function names.

## 6. Verify it actually shipped

```
gh release view v<X.Y.Z> -R tinypocket/sumatrapdf --json tagName,isDraft,assets
gh api repos/tinypocket/sumatrapdf/releases/latest -q .tag_name
```

All six assets must be listed, the release must not be a draft, and the latest
release must be the one just made — the app fetches
`releases/latest/download/update-check.txt`, so a release that is not Latest
reaches nobody.

## Notes

- `out/packages/` is not cleaned between runs, so it can hold an unreleased
  build. It is always rewritten by step 3, so this only matters if you publish
  without rebuilding.
- The settings file is generated: edit `cmd/gen-settings.ts` and run
  `bun cmd/gen-settings.ts` (it rewrites `src/Settings.h` and
  `docs/md/Advanced-options-settings.md`). Never hand-edit those two.

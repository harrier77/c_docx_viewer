# How DOCX saving works — and how to keep it working

Synthetic description of the save implementation in `d2dview.c`
(`save_docx_file`) and the rules that must be respected to avoid
regressions.

## 1. What the save does

A DOCX is a ZIP archive. The editor only ever changes `word/document.xml`.
The save procedure is:

1. **Collect new paragraphs.** For each paragraph, call
   `ld_paragraph_get_ooxml(doc, i, &xml)` which returns the raw OOXML
   fragment `<w:p>...</w:p>`. Concatenate all fragments (joined by `|`
   markers that are later discarded; the body content is what matters).

2. **Read the original archive** with `mz_zip_reader_init_file()`.

3. **Extract the old `word/document.xml`** into memory. This file contains
   the *skeleton* that must be preserved:
   - the `<?xml ...?>` declaration
   - the `<w:document xmlns:w=... xmlns:r=... xmlns:a=... ...>` root tag
     with **all** the namespace declarations (Word's SAX parser rejects
     `<w:p>` if `xmlns:w` is not declared on an ancestor)
   - the `<w:body>` open tag (possibly with attributes)

4. **Rebuild `word/document.xml`** by string surgery:
   ```
   new_doc =  [ prefix up to and including the <w:body ...> open tag ]
            + [ new paragraph fragments ]
            + "</w:body>"
            + (suffix after </w:body> — normally </w:document> —
               ONLY if the original was well-formed, i.e. contains a
               complete "</w:body>" followed by a complete
               "</w:document>"; otherwise just append "</w:document>" ourselves)
   ```
   The reason the suffix is sometimes discarded: a previously-broken save
   may have left a dangling `</w:bod` fragment. Keeping it would
   reproduce the corruption.

5. **Rebuild the ZIP archive in a temp file** (`path + ".tmp"`):
   - every original entry is copied as-is with
     `mz_zip_writer_add_from_zip_reader()`
   - `word/document.xml` is replaced with `mz_zip_writer_add_mem()`
   - the archive is finalized and closed

6. **Atomic replace.** `remove(path)` + `rename(tmp, path)`. If either
   fails, the original file is left intact and the error is reported.

This is the "clone into a new archive" strategy recommended by the miniz
documentation (scenario 1, "safest way").

## 2. Why we cannot use the "in-place" miniz API

Miniz's in-place writer (`mz_zip_writer_init_from_reader` +
`mz_zip_writer_add_mem` + `mz_zip_writer_finalize_archive`) only
**appends** files to the archive; it **cannot remove or replace** an
existing entry. Using it produces a *duplicate* `word/document.xml`
entry in the ZIP, which makes the package ambiguous and Word rejects it
with "The file is not consistent".

There is no `mz_zip_writer_remove_file()` in miniz. The only supported
ways to replace a file inside a ZIP are:
- rewrite the whole archive (what we do), or
- use a third-party ZIP tool that supports entry deletion.

## 3. Why we must keep the skeleton

The OOXML fragments returned by `ld_paragraph_get_ooxml` are bare
`<w:p>...</w:p>` elements. They carry **no** XML declaration and **no**
namespace declarations. If they are written directly into
`word/document.xml`, Word's SAX parser fails with:

```
SAXParseException: No namespace defined for w
SAXParseException: Namespace prefix w on t is not defined
```

So the save must always re-embed the fragments inside the original
`<w:document xmlns:w=...>` skeleton. Never write the raw concatenation.

## 4. Why we guard against a truncated/malformed original

A previous, buggy save can leave `word/document.xml` in a damaged state,
e.g. ending with the broken fragment `</w:bod</w:document>` (two closing
tags glued together, missing `>`). If the new code blindly preserved the
original suffix, the corruption would propagate forever.

Rule:
- Only trust the suffix when it contains a complete `</w:body>` **and**
  a complete `</w:document>` after it.
- Otherwise, discard the suffix and emit a clean `</w:document>`
  ourselves.

## 5. Regression checklist (do this after any save change)

1. **Build.** `build_d2dview.bat` must complete with `Build OK`.

2. **ZIP integrity.** The produced DOCX must:
   - contain **exactly one** `word/document.xml` entry
   - pass `python -c "import zipfile; zipfile.ZipFile('x.docx').testzip()"`
     with no error

3. **XML well-formedness.** Extract `word/document.xml` and check:
   - first line is `<?xml version="1.0" ...?>`
   - root is `<w:document ...>` with all `xmlns:*` declarations
   - every opening tag has a matching closing tag
   - last characters are `</w:body></w:document>` (possibly with
     `<w:sectPr>` in between)

   Quick Python check:
   ```python
   import zipfile, re
   from collections import Counter
   z = zipfile.ZipFile('x.docx')
   xml = z.read('word/document.xml').decode()
   tags = re.findall(r'<(/?)([A-Za-z0-9]+)(?::[A-Za-z0-9]+)?[^>]*>', xml)
   opens  = Counter(t[1] for t in tags if t[0]=='')
   closes = Counter(t[1] for t in tags if t[0]=='/')
   for k in opens:
       assert opens[k] == closes.get(k,0), f'unbalanced <{k}>'
   ```

4. **Word can open it.** Open the saved file in Microsoft Word (or
   LibreOffice). There must be no "file is corrupted" / "format error"
   dialogs.

5. **Round-trip.** Open the just-saved file in `d2dview`, save again,
   re-open in Word. No data loss, no growing XML, no duplicates.

## 6. Invariants that must hold in `save_docx_file`

| Invariant | Where enforced |
|---|---|
| One `word/document.xml` in the output ZIP | temp-file rebuild + `rename` |
| `word/document.xml` contains the XML declaration | skeleton from the original file |
| `word/document.xml` declares every namespace used | skeleton from the original file |
| New paragraphs are inside `<w:body>...</w:body>` | string surgery in step 4 |
| The closing tag `</w:document>` is present exactly once | suffix guard in step 4 |
| A save failure never destroys the original file | atomic `remove`+`rename` after the temp archive is fully finalized |
| All error paths free `para_xml`, `old_xml`, `new_doc`, `tmp_path` | each `return` in the function |

If any of these invariants is changed, re-run the regression checklist
in section 5 before committing.

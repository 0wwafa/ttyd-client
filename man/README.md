# Building the man page

`ttyd-client.1` is maintained by hand. `ttyd-client.man.md` is the markdown
source of record for documentation changes; regenerate the man page with:

```bash
go install github.com/cpuguy83/go-md2man/v2@latest
go-md2man < ttyd-client.man.md > ttyd-client.1
```

or with pandoc:

```bash
pandoc -s -t man ttyd-client.man.md -o ttyd-client.1
```

Check the result locally with:

```bash
man -l ttyd-client.1
```

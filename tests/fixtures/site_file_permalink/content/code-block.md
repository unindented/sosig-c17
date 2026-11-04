+++
title = "Code Block"
date = 2026-07-28T09:00:00-07:00
description = "Fenced code blocks in C, shell, JSX, TSX and a language with no grammar."
slug = "code-block"
tags = ["code"]
draft = false
+++

```c
#include <stdio.h>

int main(void) {
    puts("hello");
    return 0;
}
```

```sh
cc -o hello hello.c && ./hello --verbose "$USER"
```

```jsx
const Greeting = ({ name }) => <p className="hi">{name}</p>;
```

```tsx
function greet(name: string): JSX.Element { return <b>{name}</b>; }
```

```text
A block in a <language> with no grammar is only escaped.
```

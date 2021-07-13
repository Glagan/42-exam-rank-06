# Exam Rank 06

The goal of **mini_serv** is to create a simple server where client can connect and exchange messages.

The ``extract_message`` and ``str_join`` functions are given to you in the subject folder.

## Compilation

You can compile both **mini_serv** and the test **client** with:

```bash
gcc -Wall -Wextra -Werror mini_serv.c -o mini_serv
gcc -Wall -Wextra -Werror client.c -o client
```

## Usage

You first need to start **mini_serv** on a given port: ``./mini_serv 3033`` for example.

You can then connect up to ``MAX_CLIENTS`` (see ``mini_serv.c:12``) with ``./client 3033``.

## Resources

* socket
* select
* non-blocking socket

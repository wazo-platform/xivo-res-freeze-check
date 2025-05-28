# xivo-res-freeze-check

res_freeze_check is a simple asterisk module to help detect some common asterisk
freezes.

Once loaded, the module checks at regular intervals if asterisk is deadlocked or not.

It currently checks if the lock on the global channels container can be
acquired or not: if it can't, then it considers asterisk is deadlocked and aborts
the process. It does the same for the global queues container lock and all individual
queues.

## Compilation dependencies

* asterisk-dev

## Installing

```
make
make install
```

## Installing from a remote machine

```
./buildh makei
```

## Usage

### CLI

Enable/Disable locking commands

```
freeze {enable,disable}
```

Lock/Unlock the global channel container (for testing purpose)

```
freeze channel {lock,unlock}
```

Lock/Unlock the global queue container or all queues (for testing purpose)

```
freeze queue {global_lock,lock,global_unlock,unlock}
```

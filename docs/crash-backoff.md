# Crash backoff for pm children (issue #727)

This page describes how the master restarts a pm child that fails fast. The rule
applies to `pool.type = fastcgi` and to `pool.type = http-direct`, with either
`pool.executor`. The directive `pm.max_consecutive_failures` sets the limit. The
operator endpoint shows the state, as described in
[`operator-endpoint.md`](operator-endpoint.md#crash-backoff-issue-727).

## Fast failures

A child is a fast failure when it exits within 10 seconds of its start and serves
no request. The 10 second window is fixed. It is not a directive.

A child that serves a request ends the streak when it exits. A child that lives
for the whole window ends the streak. The master ends the streak while the child
still runs.

## Restart delay

The first fast failure in a row starts a new child at once. Each later fast
failure makes the master wait before it starts the next child. The wait grows
with each failure, up to the longest value in this table.

| Fast failures in a row | Wait before the next child |
|---|---|
| 1 | None. The master starts the child at once. |
| 2 | 0.5 s to 1 s |
| 3 | 1 s to 2 s |
| 4 | 2 s to 4 s |
| 5 | 4 s to 8 s |
| 6 | 8 s to 16 s |
| 7 | 16 s to 32 s |
| 8 or more | 30 s to 60 s |

Each wait is a random value in the range that the table shows. The random part
stops several pools from starting their children at the same moment.

A row applies only when its failure count is below `pm.max_consecutive_failures`,
or when that directive is `0`. With the default limit `6`, the sixth fast failure
gives up the pool. The wait after it is the row 8 value, 30 s to 60 s, at once.
So with the default limit the waits of rows 6 and 7 never happen. See
[Give-up](#give-up).

## Give-up

The directive `pm.max_consecutive_failures` sets how many fast failures in a row
a pool allows. The default is `6`. The value `0` means that the pool never gives
up.

When the streak reaches this number, the master does two things. It logs one
`ALERT` line. It sets the state of the pool to "gave up", and the operator
endpoint shows that state.

The master does not stop after a give-up. It keeps on starting children, and
the wait stays at 30 s to 60 s. One `ALERT` line is logged for each streak that
reaches the limit.

## Log lines

Each fast failure logs one `WARNING` line. The line gives the pid of the child,
its lifetime, the number of failures in a row, and the next wait in milliseconds.

When a streak ends, the master logs one `NOTICE` line. The line names the streak
length and the reason.

## Types that refuse the directive

The directive is refused on `pool.type = gateway`, `pool.type = supervisor` and
`pool.type = cron`. These types have no pm children that the rule applies to.
Their own restart rules are in [`supervisor.md`](supervisor.md) and
[`cron.md`](cron.md).

## Limits

- A reload starts the master again. The streak starts at 0 after a reload.
- The state lives in memory. A restart of php-fpm-ng clears it.
- The 10 second window is fixed. Only the limit, the `pm.max_consecutive_failures`
  value, is set by the operator.

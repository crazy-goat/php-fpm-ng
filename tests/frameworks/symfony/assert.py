#!/usr/bin/env python3
"""Data assertions for the repository-owned Symfony probe."""

from __future__ import annotations

import json
import sys
from pathlib import Path


class AssertionFailure(Exception):
    pass


def fail(message: str) -> None:
    raise AssertionFailure(message)


def load(directory: Path, name: str) -> dict:
    code = (directory / f"{name}.code").read_text().strip()
    body_path = directory / f"{name}.body"
    if code != "200":
        error = (directory / f"{name}.err").read_text(errors="replace")
        fail(f"{name}: HTTP {code}, curl error: {error.strip()}")
    try:
        value = json.loads(body_path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        fail(f"{name}: response is not JSON: {error}")
    if not isinstance(value, dict):
        fail(f"{name}: response is not an object")
    return value


def load_batch(directory: Path, count: int) -> list[dict]:
    return [load(directory, str(index)) for index in range(1, count + 1)]


def assert_mix(directory: Path, count: int) -> None:
    for index, value in enumerate(load_batch(directory, count), start=1):
        token = value.get("token")
        if value.get("id") != index:
            fail(f"mix/{index}: id is {value.get('id')!r}")
        if not value.get("ok"):
            fail(f"mix/{index}: probe returned ok=false: {value}")
        if value.get("item") != f"item-{index}":
            fail(f"mix/{index}: wrong ORM item: {value.get('item')!r}")
        if not isinstance(token, str) or not token:
            fail(f"mix/{index}: missing token")
        if value.get("db_tag") != f"t-{index}-{token}":
            fail(f"mix/{index}: wrong MySQL tag: {value.get('db_tag')!r}")
        if value.get("redis") != f"v-{index}-{token}":
            fail(f"mix/{index}: wrong direct Redis value: {value.get('redis')!r}")
        if value.get("cache") != f"c-{index}-{token}":
            fail(f"mix/{index}: wrong cache value: {value.get('cache')!r}")


def assert_session(round_one: Path, round_two: Path, count: int) -> None:
    first = load_batch(round_one, count)
    second = load_batch(round_two, count)
    first_sids: set[str] = set()
    second_sids: set[str] = set()

    for index, (one, two) in enumerate(zip(first, second), start=1):
        user = f"user{index}"
        if not one.get("ok") or one.get("sess_user") != user:
            fail(f"session/{index}/round1: wrong session data: {one}")
        if one.get("count") != 1:
            fail(f"session/{index}/round1: count is {one.get('count')!r}")
        if one.get("handler") != "user":
            fail(f"session/{index}/round1: handler is {one.get('handler')!r}")
        if not two.get("ok") or two.get("sess_user") != user:
            fail(f"session/{index}/round2: wrong session data: {two}")
        if two.get("count") != 2:
            fail(f"session/{index}/round2: count is {two.get('count')!r}")
        if one.get("sid") != two.get("sid"):
            fail(f"session/{index}: session id changed between rounds")
        first_sids.add(str(one.get("sid")))
        second_sids.add(str(two.get("sid")))

    if len(first_sids) != count or len(second_sids) != count:
        fail(f"session: session ids are not disjoint ({first_sids!r})")


def assert_auth(round_one: Path, round_two: Path, users: list[str]) -> None:
    first = [load(round_one, str(index)) for index in range(1, len(users) + 1)]
    second = [load(round_two, str(index)) for index in range(1, len(users) + 1)]
    for index, (user, one, two) in enumerate(zip(users, first, second), start=1):
        if not one.get("ok") or one.get("user") != user or one.get("auth_header") != "yes":
            fail(f"auth/{index}/round1: wrong authenticated response: {one}")
        if not two.get("ok") or two.get("user") != user or two.get("auth_header") != "no":
            fail(f"auth/{index}/round2: session did not restore identity: {two}")


def assert_identity(directory: Path, count: int) -> None:
    values = load_batch(directory, count)
    for index, value in enumerate(values, start=1):
        if not value.get("ok") or value.get("user") is not None:
            fail(f"identity/{index}: unexpected request state: {value}")
        if value.get("stack_depth") != 1:
            fail(f"identity/{index}: RequestStack depth is {value.get('stack_depth')!r}")

    for field in ("kernel_oid", "container_oid", "request_oid", "entity_manager_oid", "connection_oid"):
        values_for_field = {value.get(field) for value in values}
        if len(values_for_field) != count:
            fail(f"identity: {field} is not distinct per concurrent request: {values_for_field!r}")


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: assert.py SCENARIO DIRECTORY [DIRECTORY ...]", file=sys.stderr)
        return 2

    scenario = sys.argv[1]
    try:
        if scenario == "mix":
            assert_mix(Path(sys.argv[2]), int(sys.argv[3]))
        elif scenario == "session":
            assert_session(Path(sys.argv[2]), Path(sys.argv[3]), int(sys.argv[4]))
        elif scenario == "auth":
            users = sys.argv[4:]
            assert_auth(Path(sys.argv[2]), Path(sys.argv[3]), users)
        elif scenario == "identity":
            assert_identity(Path(sys.argv[2]), int(sys.argv[3]))
        else:
            fail(f"unknown scenario {scenario!r}")
    except (AssertionFailure, OSError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1

    print("PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

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


def load(directory: Path, name: str, expected_environment: str | None = None) -> dict:
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
    if expected_environment is not None and value.get("environment") != expected_environment:
        fail(f"{name}: expected APP_ENV={expected_environment!r}, got {value.get('environment')!r}")
    return value


def load_batch(directory: Path, count: int, expected_environment: str | None = None) -> list[dict]:
    return [load(directory, str(index), expected_environment) for index in range(1, count + 1)]


def assert_mix(directory: Path, count: int, environment: str | None) -> None:
    for index, value in enumerate(load_batch(directory, count, environment), start=1):
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


def assert_session(round_one: Path, round_two: Path, count: int, environment: str | None) -> None:
    first = load_batch(round_one, count, environment)
    second = load_batch(round_two, count, environment)
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


def assert_auth(round_one: Path, round_two: Path, users: list[str], environment: str | None) -> None:
    first = [load(round_one, str(index), environment) for index in range(1, len(users) + 1)]
    second = [load(round_two, str(index), environment) for index in range(1, len(users) + 1)]
    for index, (user, one, two) in enumerate(zip(users, first, second), start=1):
        if not one.get("ok") or one.get("user") != user or one.get("auth_header") != "yes":
            fail(f"auth/{index}/round1: wrong authenticated response: {one}")
        if not two.get("ok") or two.get("user") != user or two.get("auth_header") != "no":
            fail(f"auth/{index}/round2: session did not restore identity: {two}")


def assert_identity(directory: Path, count: int, environment: str | None) -> None:
    values = load_batch(directory, count, environment)
    for index, value in enumerate(values, start=1):
        if not value.get("ok") or value.get("user") is not None:
            fail(f"identity/{index}: unexpected request state: {value}")
        if value.get("stack_depth") != 1:
            fail(f"identity/{index}: RequestStack depth is {value.get('stack_depth')!r}")

    for field in ("kernel_oid", "container_oid", "request_oid", "entity_manager_oid", "connection_oid"):
        values_for_field = {f"{value.get('pid')}:{value.get(field)}" for value in values}
        if len(values_for_field) != count:
            fail(f"identity: {field} is not distinct per concurrent request: {values_for_field!r}")


def assert_twig(directory: Path, users: list[str], environment: str | None) -> None:
    values = [load(directory, str(index), environment) for index in range(1, len(users) + 1)]
    for index, (user, value) in enumerate(zip(users, values), start=1):
        marker = f"marker-{index}"
        expected_value = f"value-{index}"
        html = value.get("html", "")
        if not value.get("ok") or value.get("user") != user:
            fail(f"twig/{index}: wrong user or ok flag: {value}")
        if value.get("marker") != marker or value.get("value") != expected_value:
            fail(f"twig/{index}: wrong controller data: {value}")
        if f'data-user="{user}"' not in html:
            fail(f"twig/{index}: app.user did not render as {user!r}: {html!r}")
        if f'data-marker="{marker}"' not in html or f'data-value="{expected_value}"' not in html:
            fail(f"twig/{index}: request data leaked in rendered template: {html!r}")
        if not value.get("gate_released"):
            fail(f"twig/{index}: Twig gate did not release")


def assert_form(directory: Path, names: list[str], environment: str | None) -> None:
    values = [load(directory, str(index), environment) for index in range(1, len(names) + 1)]
    for index, (name, value) in enumerate(zip(names, values), start=1):
        # TextType maps an empty submit to null, so normalize before comparing.
        submitted = value.get("submitted_name") or ""
        expected_valid = name != ""
        if submitted != name:
            fail(f"form/{index}: submitted value changed: {value}")
        if value.get("valid") != expected_valid or not value.get("gate_released"):
            fail(f"form/{index}: wrong validation result: {value}")
        if expected_valid and value.get("errors"):
            fail(f"form/{index}: valid form returned errors: {value}")
        if not expected_valid and not value.get("errors"):
            fail(f"form/{index}: invalid form returned no errors: {value}")


def assert_messenger(directory: Path, values_to_send: list[str], environment: str | None) -> None:
    values = [load(directory, str(index), environment) for index in range(1, len(values_to_send) + 1)]
    for index, (sent, value) in enumerate(zip(values_to_send, values), start=1):
        if not value.get("ok") or value.get("value") != sent:
            fail(f"messenger/{index}: wrong dispatch response: {value}")
        if value.get("handled") != f"handled:{sent}:released":
            fail(f"messenger/{index}: synchronous handler result is wrong: {value}")


def assert_rss(directory: Path, count: int, environment: str | None, max_growth_kb: int) -> None:
    values = load_batch(directory, count, environment)
    pids = {value.get("pid") for value in values}
    if len(pids) != 1:
        fail(f"rss: sequential run used multiple workers: {pids!r}")
    rss = [value.get("rss_kb") for value in values]
    if not all(isinstance(value, int) and value > 0 for value in rss):
        fail("rss: /proc/self/status did not provide a positive VmRSS value")
    growth = max(rss) - rss[0]
    if growth > max_growth_kb:
        fail(f"rss: max growth {growth} KiB exceeds {max_growth_kb} KiB (first={rss[0]}, last={rss[-1]}, max={max(rss)})")
    for index, value in enumerate(values, start=1):
        if not value.get("ok"):
            fail(f"rss/{index}: identity response returned ok=false: {value}")
    print(f"PASS first_rss_kb={rss[0]} last_rss_kb={rss[-1]} max_rss_kb={max(rss)} growth_kb={growth}")


def assert_deploy(directory: Path, expected_environment: str | None) -> None:
    first = load(directory, "first", expected_environment)
    immediate = load(directory, "immediate", expected_environment)
    eventual = load(directory, "eventual", expected_environment)
    if first.get("version") != "release-1":
        fail(f"revalidate: initial marker is wrong: {first}")
    if immediate.get("version") not in {"release-1", "release-2"}:
        fail(f"revalidate: immediate marker is wrong: {immediate}")
    if eventual.get("version") != "release-2":
        fail(f"revalidate: changed marker never became active: {eventual}")
    print(f"PASS immediate={immediate.get('version')} first_pid={first.get('pid')} eventual_pid={eventual.get('pid')} (same pid is acceptable: the revalidated code must run, the worker need not restart)")


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: assert.py SCENARIO DIRECTORY [ARGS ...]", file=sys.stderr)
        return 2

    scenario = sys.argv[1]
    try:
        if scenario == "mix":
            assert_mix(Path(sys.argv[2]), int(sys.argv[3]), sys.argv[4] if len(sys.argv) > 4 else None)
        elif scenario == "session":
            assert_session(Path(sys.argv[2]), Path(sys.argv[3]), int(sys.argv[4]), sys.argv[5] if len(sys.argv) > 5 else None)
        elif scenario == "auth":
            environment = sys.argv[4]
            assert_auth(Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[5:], environment)
        elif scenario == "identity":
            assert_identity(Path(sys.argv[2]), int(sys.argv[3]), sys.argv[4] if len(sys.argv) > 4 else None)
        elif scenario == "twig":
            assert_twig(Path(sys.argv[2]), sys.argv[4:], sys.argv[3])
        elif scenario == "form":
            assert_form(Path(sys.argv[2]), sys.argv[4:], sys.argv[3])
        elif scenario == "messenger":
            assert_messenger(Path(sys.argv[2]), sys.argv[4:], sys.argv[3])
        elif scenario == "rss":
            assert_rss(Path(sys.argv[2]), int(sys.argv[3]), sys.argv[4], int(sys.argv[5]))
        elif scenario == "deploy":
            assert_deploy(Path(sys.argv[2]), sys.argv[3] if len(sys.argv) > 3 else None)
        else:
            fail(f"unknown scenario {scenario!r}")
    except (AssertionFailure, OSError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1

    print("PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

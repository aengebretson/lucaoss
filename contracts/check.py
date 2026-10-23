#!/usr/bin/env python3
"""Developer-only schema/catalog checks; never part of Luca's C++ runtime."""
import argparse
import copy
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def load(relative):
    return json.loads((ROOT / relative).read_text())


def standalone(schema, definitions):
    needed = set()

    def visit(value):
        if isinstance(value, dict):
            if "$ref" in value:
                reference = value["$ref"]
                if not reference.startswith("#/$defs/"):
                    raise ValueError(f"External reference is not allowed: {reference}")
                name = reference.removeprefix("#/$defs/")
                if name not in needed:
                    needed.add(name)
                    visit(definitions[name])
            for child in value.values():
                visit(child)
        elif isinstance(value, list):
            for child in value:
                visit(child)

    visit(schema)
    return {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        **copy.deepcopy(schema),
        "$defs": {name: copy.deepcopy(definitions[name]) for name in sorted(needed)},
    }


def build_catalog(schema):
    definitions = schema["$defs"]
    return {
        "api_version": "luca.application/1",
        "tools": [
            {
                "name": "luca_" + tool["operation"],
                "description": tool["description"],
                "inputSchema": standalone(
                    definitions[tool["operation"] + "_request"], definitions
                ),
                "outputSchema": standalone(
                    definitions[tool["operation"] + "_response"], definitions
                ),
                "annotations": tool["annotations"],
            }
            for tool in schema["x-luca-mcp-tools"]
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write-catalog", action="store_true")
    args = parser.parse_args()
    schema = load("application/v1.schema.json")
    generated = build_catalog(schema)
    if args.write_catalog:
        (ROOT / "mcp/v1.tools.json").write_text(
            json.dumps(generated, indent=2, ensure_ascii=False) + "\n"
        )
    if load("mcp/v1.tools.json") != generated:
        raise SystemExit("MCP catalog drift; run contracts/check.py --write-catalog")

    # Optional development dependency only; no production/build discovery added.
    try:
        from jsonschema import Draft202012Validator, FormatChecker
    except ImportError as error:
        raise SystemExit("Install developer dependency jsonschema[format]==4.26.0") from error

    Draft202012Validator.check_schema(schema)
    full = Draft202012Validator(schema, format_checker=FormatChecker())
    tools = {tool["name"].removeprefix("luca_"): tool for tool in generated["tools"]}
    if len(tools) != len(generated["tools"]):
        raise SystemExit("Duplicate tool names")
    for tool in tools.values():
        for key in ("inputSchema", "outputSchema"):
            Draft202012Validator.check_schema(tool[key])

    positive = load("examples/application-v1.json")
    seen = set()
    for exchange in positive:
        full.validate(exchange)
        name = exchange["operation"]
        seen.add(name)
        for part, key in (("request", "inputSchema"), ("response", "outputSchema")):
            Draft202012Validator(tools[name][key], format_checker=FormatChecker()).validate(
                exchange[part]
            )
    if seen != set(tools):
        raise SystemExit("Every tool needs at least one valid exchange")

    negative = load("examples/invalid-v1.json")
    for case in negative:
        key = "inputSchema" if case["part"] == "request" else "outputSchema"
        validator = Draft202012Validator(
            tools[case["operation"]][key], format_checker=FormatChecker()
        )
        if validator.is_valid(case["value"]):
            raise SystemExit("Invalid example accepted: " + case["reason"])

    print(
        f"PASS: {len(tools)} tools, {len(tools) * 2} self-contained schemas, "
        f"{len(positive)} valid exchanges, {len(negative)} rejected cases; no catalog drift"
    )


if __name__ == "__main__":
    main()

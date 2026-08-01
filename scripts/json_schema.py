"""Small dependency-free validator for the JSON Schema subset used by CI."""

import math
import re


class ValidationError(AssertionError):
    pass


def _resolve(root, reference):
    if not reference.startswith("#/"):
        raise ValidationError(f"external schema reference is unsupported: {reference}")
    value = root
    for component in reference[2:].split("/"):
        key = component.replace("~1", "/").replace("~0", "~")
        value = value[key]
    return value


def _matches_type(instance, expected):
    if expected == "object":
        return isinstance(instance, dict)
    if expected == "array":
        return isinstance(instance, list)
    if expected == "string":
        return isinstance(instance, str)
    if expected == "boolean":
        return isinstance(instance, bool)
    if expected == "integer":
        return isinstance(instance, int) and not isinstance(instance, bool)
    if expected == "number":
        return isinstance(instance, (int, float)) and not isinstance(
            instance, bool) and math.isfinite(instance)
    if expected == "null":
        return instance is None
    raise ValidationError(f"unsupported schema type: {expected}")


def validate(instance, schema, root=None, path="$"):
    if root is None:
        root = schema
    if "$ref" in schema:
        return validate(instance, _resolve(root, schema["$ref"]), root, path)
    if "oneOf" in schema:
        errors = []
        matches = 0
        for candidate in schema["oneOf"]:
            try:
                validate(instance, candidate, root, path)
                matches += 1
            except ValidationError as error:
                errors.append(str(error))
        if matches != 1:
            raise ValidationError(
                f"{path}: expected exactly one matching schema, got {matches}; "
                f"errors={errors}")
        return
    if "const" in schema and instance != schema["const"]:
        raise ValidationError(
            f"{path}: expected constant {schema['const']!r}, got {instance!r}")
    if "enum" in schema and instance not in schema["enum"]:
        raise ValidationError(
            f"{path}: value {instance!r} is not in {schema['enum']!r}")

    expected_type = schema.get("type")
    if expected_type is not None:
        choices = expected_type if isinstance(expected_type, list) \
            else [expected_type]
        if not any(_matches_type(instance, choice) for choice in choices):
            raise ValidationError(
                f"{path}: expected type {expected_type!r}, got "
                f"{type(instance).__name__}")

    if isinstance(instance, dict):
        properties = schema.get("properties", {})
        for name in schema.get("required", []):
            if name not in instance:
                raise ValidationError(f"{path}: missing required property {name}")
        if schema.get("additionalProperties") is False:
            unknown = sorted(set(instance) - set(properties))
            if unknown:
                raise ValidationError(
                    f"{path}: unknown properties: {', '.join(unknown)}")
        for name, value in instance.items():
            if name in properties:
                validate(value, properties[name], root, f"{path}.{name}")

    if isinstance(instance, list):
        if len(instance) < schema.get("minItems", 0):
            raise ValidationError(f"{path}: too few array items")
        if "maxItems" in schema and len(instance) > schema["maxItems"]:
            raise ValidationError(f"{path}: too many array items")
        if schema.get("uniqueItems"):
            encoded = [repr(value) for value in instance]
            if len(set(encoded)) != len(encoded):
                raise ValidationError(f"{path}: array items must be unique")
        item_schema = schema.get("items")
        if item_schema is not None:
            for index, value in enumerate(instance):
                validate(value, item_schema, root, f"{path}[{index}]")

    if isinstance(instance, str):
        if len(instance) < schema.get("minLength", 0):
            raise ValidationError(f"{path}: string is too short")
        if "maxLength" in schema and len(instance) > schema["maxLength"]:
            raise ValidationError(f"{path}: string is too long")
        if "pattern" in schema and re.search(schema["pattern"], instance) is None:
            raise ValidationError(
                f"{path}: string does not match {schema['pattern']!r}")

    if isinstance(instance, (int, float)) and not isinstance(instance, bool):
        if "minimum" in schema and instance < schema["minimum"]:
            raise ValidationError(f"{path}: value is below minimum")
        if "maximum" in schema and instance > schema["maximum"]:
            raise ValidationError(f"{path}: value is above maximum")
        if "exclusiveMinimum" in schema and \
                instance <= schema["exclusiveMinimum"]:
            raise ValidationError(f"{path}: value is below exclusive minimum")


import json
import os
import sys
import urllib.error
import urllib.request


def main() -> int:
    if len(sys.argv) != 2:
        return 1

    prompt_path = sys.argv[1]
    with open(prompt_path, "r", encoding="utf-8") as prompt_file:
        prompt = prompt_file.read()

    base_url = os.environ.get("MISRA_AI_BASE_URL", "https://api.openai.com/v1")
    model = os.environ.get("MISRA_AI_MODEL", "gpt-4o-mini")

    if "localhost:11434" in base_url or "127.0.0.1:11434" in base_url:
        return call_ollama(base_url, model, prompt)

    api_key = os.environ.get("MISRA_AI_API_KEY") or os.environ.get("OPENAI_API_KEY")
    if not api_key:
        print("MISRA AI error: OPENAI_API_KEY or MISRA_AI_API_KEY is not set.", file=sys.stderr)
        return 1

    return call_openai_compatible(base_url, model, api_key, prompt)


def call_openai_compatible(base_url: str, model: str, api_key: str, prompt: str) -> int:
    url = base_url.rstrip("/") + "/chat/completions"

    body = {
        "model": model,
        "messages": [
            {
                "role": "system",
                "content": (
                    "You are a C/C++ coding assistant. Return only the requested "
                    "MISRA correction blocks. Do not include file names, paths, "
                    "AST details, explanations, or full corrected source code."
                ),
            },
            {"role": "user", "content": prompt},
        ],
        "temperature": 0.2,
    }

    request = urllib.request.Request(
        url,
        data=json.dumps(body).encode("utf-8"),
        headers={
            "Authorization": "Bearer " + api_key,
            "Content-Type": "application/json",
        },
        method="POST",
    )

    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            data = json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as error:
        print("MISRA AI error:", error.read().decode("utf-8", errors="replace"), file=sys.stderr)
        return 1
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as error:
        print("MISRA AI error:", str(error), file=sys.stderr)
        return 1

    content = data.get("choices", [{}])[0].get("message", {}).get("content", "")
    sys.stdout.write(content)
    return 0


def call_ollama(base_url: str, model: str, prompt: str) -> int:
    url = base_url.rstrip("/")
    if url.endswith("/v1"):
        url = url[:-3]
    url += "/api/generate"

    body = {
        "model": model,
        "prompt": prompt,
        "stream": False,
        "options": {
            "temperature": 0.2
        }
    }

    request = urllib.request.Request(
        url,
        data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )

    try:
        with urllib.request.urlopen(request, timeout=120) as response:
            data = json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as error:
        print("MISRA AI error:", error.read().decode("utf-8", errors="replace"), file=sys.stderr)
        return 1
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as error:
        print("MISRA AI error:", str(error), file=sys.stderr)
        return 1

    content = data.get("response", "")
    sys.stdout.write(content)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

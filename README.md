# AI Desktop Companion

A small bird that lives on your desktop. A locally-run LLM drives its personality, memory, and evolution over time.

## Features

- **Animations** — Weight-driven animation system. The bird idles, flies, sleeps, pecks, and dances if you ask nicely.
- **Interactivity** — Pet, poke, or talk to the bird. Reacts relatively quickly and remembers your interactions.. so don't poke too much.
- **Evolution** — Personality and animation weights shift slowly over time from user interactions and environment cues.
- **Memory** — A running history log shapes the LLM context. Gives the bird continuity across sessions.

## Requirements

- Windows 10 or later
- [Ollama](https://ollama.com) with at least one model pulled

## Setup

**The app**

1. Download and extract the zip from the [latest release](https://github.com/ivanOzerets/AI-Desktop-Companion/releases)
2. Extract the zip anywhere you want to run it from
3. Double-click `ai-desktop-companion.exe` — the bird should fly in

**Ollama**

1. Download and install [Ollama](https://ollama.com)
2. Pull a model — `ollama pull gemma2:9b`
3. The bird auto-detects your latest model, or pin a specific one in `config.json`

## Controls & config

`Ctrl + Shift + Space` — open a spotlight to talk to the bird  
`Click the bird` — sends it flying to a new ledge  
`Pet the bird` — drag your cursor back and forth over it  
`config.json` — pin a model, adjust max tokens, and override settings  
`weights.json` — manually adjust how often each animation plays  

## Lightweight model options

`gemma2:9b` — 6 GB — Pretty good. Can be actually funny sometimes.  
`llama3.1:8b` — 5 GB — Not bad. Can get repetitive at times.  
`mistral` — 4 GB — Meh, just alright. Nothing special.  
`phi3:mini` — 2 GB — Like the first one, wouldn't recommend.  
`llama3.2` — 2 GB — Not that good. Wouldn't recommend.  
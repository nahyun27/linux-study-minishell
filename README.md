# nsh — A Simple Unix Shell

A lightweight Unix shell implemented in C, supporting command execution, I/O redirection, multi-stage pipes, and background execution.

---

## Build

```bash
gcc -o nsh nsh.c
./nsh
```

---

## Features

### Basic Command Execution
명령어를 입력하면 자식 프로세스를 생성하여 실행한다. 부모 프로세스는 자식이 종료될 때까지 대기한다.

```
nsh> grep int nsh.c
nsh> ps
```

### Background Execution (`&`)
명령어 끝에 `&`를 붙이면 부모 프로세스가 기다리지 않고 바로 다음 명령을 받는다.

```
nsh> grep "if.*NULL" nsh.c &
```

### I/O Redirection
`<`, `>` 기호로 표준 입출력을 파일로 전환한다.

```
nsh> grep "int " < nsh.c          # stdin을 파일에서 읽기
nsh> ls -l > delme                # stdout을 파일로 저장
nsh> sort < delme > delme2        # 입출력 동시 리다이렉션
```

### Multi-stage Pipes (`|`)
파이프를 재귀적으로 처리하므로 단계 수 제한 없이 사용 가능하다.

```
nsh> ps -A | grep -i system
nsh> ps -A | grep -i system | awk '{print $1,$4}'
nsh> cat nsh.c | head -6 | tail -5 | head -1
```

### Combination (Pipe + Redirection)
파이프와 리다이렉션을 함께 사용할 수 있다.

```
nsh> sort < nsh.c | grep "int " | awk '{print $1,$2}' > delme3
```

### Quote Handling
싱글쿼트(`'`)와 더블쿼트(`"`) 내부의 공백과 특수문자를 하나의 토큰으로 처리한다.

```
nsh> grep "int " < nsh.c
nsh> awk '{print $1,$2}'
```

### Exit
```
nsh> exit
```

---

## Implementation

### Function Overview

| 함수 | 역할 |
|------|------|
| `read_input()` | 명령어 파싱. 공백/따옴표/`&` 처리 |
| `has_pipe()` | args에 `\|` 토큰 존재 여부 확인 |
| `remove_redir_tokens()` | args에서 리다이렉션 심볼과 파일명을 shift로 제거 |
| `apply_redirection()` | `<`, `>` 처리 후 `dup2()`로 fd 교체 |
| `execute_single()` | 단일 명령 실행 (리다이렉션 포함) |
| `execute_pipe_recursive()` | 재귀적 다단계 파이프 실행 |
| `execute()` | fork 후 single/pipe 분기, 백그라운드 처리 |

### Multi-pipe Recursive Strategy

`command_1 | command_2 | ... | command_n` 형태의 파이프를 재귀로 처리한다.

```
execute_pipe_recursive([a | b | c])
  ├─ fork → 손자1: execvp(a)           [stdout → pipe1]
  └─ stdin ← pipe1
     execute_pipe_recursive([b | c])
       ├─ fork → 손자2: execvp(b)      [stdout → pipe2]
       └─ stdin ← pipe2
          execute_single([c])          ← base case: execvp(c)
```

### Redirection Token Removal

`apply_redirection()`은 NULL hole을 남기지 않도록 `remove_redir_tokens()`로 토큰을 즉시 shift한다.

```
before: ["sort", "<", "nsh.c", "|", "grep", NULL]
after:  ["sort", "|", "grep", NULL]
```

---

## Limitations

- 명령어 최대 길이: 80자 (`MAX_LINE`)
- 중첩 따옴표 미지원
- `>>` (append redirection) 미지원

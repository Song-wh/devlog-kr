; clock_chain: 의존 관계가 있는 정수 덧셈을 100개씩 iterations번 실행한다.
; 각 add는 앞 add의 결과(rax)를 기다리므로 1사이클에 1개씩만 실행된다(지연 1사이클).
; 그래서 걸린 시간 = 덧셈 수 / 클럭. 실행 시간을 재면 실제 클럭을 추정할 수 있다.
; C/C++로 쓰면 컴파일러가 덧셈을 하나로 합쳐 버리므로 어셈블리로 둔다.
;
;   uint64_t clock_chain(uint64_t iterations);   // Windows x64: rcx = iterations, 반환 rax
.code
clock_chain PROC
    xor     eax, eax
    mov     edx, 1
chain_loop:
    REPT 100
    add     rax, rdx
    ENDM
    dec     rcx
    jnz     chain_loop
    ret
clock_chain ENDP
END

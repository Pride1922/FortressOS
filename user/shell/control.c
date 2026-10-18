#include "control.h"
#include "parser.h"
#include "expand.h"
#include "vars.h"
#include "io.h"

enum operation { SIMPLE,JUMP,BRANCH,ZERO,WHILE_ENTER,FOR_ENTER,FOR_NEXT,SAVE_RESULT,LOOP_EXIT };
typedef struct { unsigned start,end;enum token_type type;char word[MAX_VAR_NAME];bool quoted,operand; } ctok;
typedef struct { enum operation op;unsigned start,length;int target,next;char name[MAX_VAR_NAME];bool implicit; } instruction;
typedef struct {
    int end,next,result;unsigned count,index;char name[MAX_VAR_NAME];
    char values[4096];const char *argv[MAX_EXPANDED_ARGS];
} loop_frame;
typedef struct {
    char source[CONTROL_SOURCE_CAP],command[LINE_CAP];
    instruction code[CONTROL_MAX_NODES];
    loop_frame frames[CONTROL_MAX_DEPTH];
    unsigned ncode,depth,pc,levels;
    bool running,again_requested,ready;
} control_context_t;
static control_context_t contexts[CONTROL_MAX_SOURCES+1];
static unsigned selected;
static ctok tokens[CONTROL_MAX_TOKENS];
static lexer_t lexer;
static parse_tree_t syntax;
static expanded_cmd_t expanded;
static unsigned ntokens,pos;
static int problem;
static const char *error;
unsigned control_stop;
bool control_select(unsigned index) {
    if(index>CONTROL_MAX_SOURCES) return false;
    selected=index;control_stop=0;return true;
}
#define source (contexts[selected].source)
#define command (contexts[selected].command)
#define code (contexts[selected].code)
#define frames (contexts[selected].frames)
#define ncode (contexts[selected].ncode)
#define depth (contexts[selected].depth)
#define pc (contexts[selected].pc)
#define levels (contexts[selected].levels)
#define running (contexts[selected].running)
#define again_requested (contexts[selected].again_requested)
#define ready (contexts[selected].ready)

const char *control_error(void) {return error ? error : "syntax error";}
bool control_empty(void) {return ready && !ncode;}
static bool bad(const char *message) {problem=-2;error=message;return false;}
static bool incomplete(void) {problem=-1;return false;}
static bool word(const char *s) {return pos<ntokens && tokens[pos].type==TOK_WORD && !tokens[pos].quoted && equal(tokens[pos].word,s);}
static bool eof(void) {return pos>=ntokens || tokens[pos].type==TOK_EOF;}
static void separators(void) {while(!eof() && tokens[pos].type==TOK_SEMI) pos++;}
static int emit(enum operation op) {
    if(ncode==CONTROL_MAX_NODES) {bad("control program exceeds 256 instructions");return -1;}
    int i=(int)ncode++;code[i]=(instruction){.op=op,.target=-1,.next=-1};return i;
}
static bool copy_span(unsigned start,unsigned end,bool header) {
    unsigned prefix=header ? 8 : 0;
    if(end<start || end-start+prefix>=sizeof(command)) return bad("command exceeds 4096 bytes");
    const char *dummy="__for__ ";
    for(unsigned i=0;i<prefix;i++) command[i]=dummy[i];
    for(unsigned i=start;i<end;i++) command[prefix+i-start]=source[i];
    command[prefix+end-start]=0;return true;
}
static bool expect(const char *keyword) {
    separators();if(eof()) return incomplete();
    if(!word(keyword)) return bad("unexpected control keyword");
    pos++;return true;
}
enum stops { STOP_THEN=1,STOP_BRANCH=2,STOP_FI=4,STOP_DO=8,STOP_DONE=16 };
static bool stopped(unsigned stop) {
    return ((stop & STOP_THEN) && word("then")) ||
        ((stop & STOP_BRANCH) && (word("elif") || word("else") || word("fi"))) ||
        ((stop & STOP_FI) && word("fi")) || ((stop & STOP_DO) && word("do")) ||
        ((stop & STOP_DONE) && word("done"));
}
static bool list(unsigned stop,unsigned nesting);
static bool conditional(unsigned nesting) {
    int ends=-1;
    pos++; /* if */
    for(;;) {
        if(!list(STOP_THEN,nesting+1) || !expect("then")) return false;
        int branch=emit(BRANCH);if(branch<0) return false;
        if(!list(STOP_BRANCH,nesting+1)) return false;
        int jump=emit(JUMP);if(jump<0) return false;
        code[jump].target=ends;ends=jump;code[branch].target=(int)ncode;
        if(word("elif")) {pos++;continue;}
        if(word("else")) {
            pos++;if(!list(STOP_FI,nesting+1)) return false;
        } else if(emit(ZERO)<0) return false;
        if(!expect("fi")) return false;
        while(ends>=0) {int previous=code[ends].target;code[ends].target=(int)ncode;ends=previous;}
        return true;
    }
}
static bool loop(bool for_loop,unsigned nesting) {
    pos++;
    int enter=emit(for_loop ? FOR_ENTER : WHILE_ENTER);if(enter<0) return false;
    int condition=(int)ncode,branch=-1;
    if(for_loop) {
        if(eof()) return incomplete();
        if(tokens[pos].type!=TOK_WORD || tokens[pos].quoted || !vars_is_valid_name(tokens[pos].word))
            return bad("for requires a variable name");
        for(unsigned i=0;i<MAX_VAR_NAME;i++) code[enter].name[i]=tokens[pos].word[i];
        pos++;
        if(word("in")) {
            pos++;unsigned start=eof() ? lexer.pos : tokens[pos].start;
            while(!eof() && tokens[pos].type==TOK_WORD) pos++;
            if(eof()) return incomplete();
            if(tokens[pos].type!=TOK_SEMI) return bad("invalid for list");
            unsigned end=tokens[pos].start;
            if(!copy_span(start,end,true)) return false;
            if(parser_parse(command,&syntax)!=PARSE_OK || syntax.cmd_count!=1 || syntax.cmds[0].redir_count)
                return bad("invalid for words");
            code[enter].start=start;code[enter].length=end-start;
        } else code[enter].implicit=true;
        if(!expect("do")) return false;
        condition=emit(FOR_NEXT);if(condition<0) return false;
    } else {
        if(!list(STOP_DO,nesting+1) || !expect("do")) return false;
        branch=emit(BRANCH);if(branch<0) return false;
    }
    code[enter].next=condition;
    if(!list(STOP_DONE,nesting+1) || !expect("done")) return false;
    if(emit(SAVE_RESULT)<0) return false;
    int jump=emit(JUMP);if(jump<0) return false;code[jump].target=condition;
    int end=emit(LOOP_EXIT);if(end<0) return false;
    code[enter].target=end;
    if(branch>=0) code[branch].target=end;
    if(for_loop) code[condition].target=end;
    return true;
}
static bool list(unsigned stop,unsigned nesting) {
    if(nesting>CONTROL_MAX_DEPTH) return bad("control nesting exceeds eight");
    bool any=false;
    for(;;) {
        separators();
        if(eof()) return stop ? incomplete() : true;
        if(stopped(stop)) return any ? true : bad("empty condition or body");
        any=true;
        if(word("if") || word("while") || word("for")) {
            bool ok=word("if") ? conditional(nesting) : loop(word("for"),nesting);
            if(!ok) return false;
            if(!eof() && tokens[pos].type!=TOK_SEMI) return bad("expected separator after compound command");
            continue;
        }
        if(word("then") || word("elif") || word("else") || word("fi") || word("do") || word("done") ||
           word("until") || word("case") || word("esac") || word("function")) return bad("unexpected or unsupported control keyword");
        unsigned start=tokens[pos].start;
        unsigned first=pos;
        while(!eof() && tokens[pos].type!=TOK_SEMI) {
            if(pos>first && (tokens[pos-1].type==TOK_AND || tokens[pos-1].type==TOK_OR ||
                tokens[pos-1].type==TOK_PIPE || tokens[pos-1].type==TOK_BANG) &&
                (word("if") || word("for") || word("while")))
                return bad("compound commands after list operators are not supported");
            pos++;
        }
        unsigned end=eof() ? (unsigned)length(source) : tokens[pos].start;
        if(!copy_span(start,end,false)) return false;
        enum parse_result r=parser_parse(command,&syntax);
        if(r==PARSE_INCOMPLETE) return eof() ? incomplete() : bad("incomplete command before separator");
        if(r!=PARSE_OK && r!=PARSE_EMPTY) return bad(syntax.error_msg);
        if(r==PARSE_OK) {
            int n=emit(SIMPLE);if(n<0) return false;
            code[n].start=start;code[n].length=end-start;
        }
    }
}
int control_compile(const char *text) {
    ncode=ntokens=pos=0;problem=0;error=NULL;ready=running=false;
    unsigned size=0;
    while(text[size]) {
        if(size==CONTROL_SOURCE_CAP-1) {bad("compound unit exceeds 16 KiB");return problem;}
        source[size]=text[size];size++;
    }
    source[size]=0;lexer_init(&lexer,source);
    for(;;) {
        if(ntokens==CONTROL_MAX_TOKENS) {bad("control unit exceeds 1024 tokens");return problem;}
        ctok *t=&tokens[ntokens++];*t=(ctok){.start=(unsigned)lexer.pos};token_t token;
        t->type=lexer_next(&lexer,&token);t->end=(unsigned)lexer.pos;t->quoted=token.has_quotes;
        if(t->type==TOK_REDIR) t->operand=token.redir_op==REDIR_IN || token.redir_op==REDIR_OUT ||
            token.redir_op==REDIR_APP || ((token.redir_op==REDIR_DUP_IN || token.redir_op==REDIR_DUP_OUT) && token.redir_dup_fd<0);
        if(t->type==TOK_SEMI && token.value[0]=='\n' && ntokens>1) {
            enum token_type prior=tokens[ntokens-2].type;
            if(prior==TOK_AND || prior==TOK_OR || prior==TOK_PIPE ||
               (prior==TOK_REDIR && tokens[ntokens-2].operand)) {ntokens--;continue;}
        }
        if(t->type==TOK_WORD && token.len<sizeof(t->word))
            for(size_t i=0;i<=token.len;i++) t->word[i]=token.value[i];
        if(t->type==TOK_EOF) break;
    }
    if(lexer.status!=LEX_OK) return -1;
    if(!list(0,0)) return problem;
    ready=true;return 0;
}
int control_request(bool again,int argc,char **argv) {
    unsigned count=1;
    if(!running || !depth || argc>2) {puts_err("loop control: outside loop or invalid operands\n");return 1;}
    if(argc==2) {
        count=0;if(!argv[1][0]) return 2;
        for(const char *s=argv[1];*s;s++) {
            if(*s<'0' || *s>'9' || count>CONTROL_MAX_DEPTH) return 2;
            count=count*10+(unsigned)(*s-'0');
        }
    }
    if(!count || count>depth) {puts_err("loop control: invalid depth\n");return 1;}
    again_requested=again;levels=count;control_stop=1;return 0;
}
static bool for_values(const instruction *op,loop_frame *frame,int status) {
    if(op->implicit) {
        const char *s="__for__ \"$@\"";unsigned i=0;do {command[i]=s[i];} while(s[i++]);
    } else if(!copy_span(op->start,op->start+op->length,true)) return false;
    if(parser_parse(command,&syntax)!=PARSE_OK || expand_command_checked(&syntax.cmds[0],status,&expanded)) return false;
    frame->count=0;frame->index=0;unsigned used=0;
    for(int i=1;i<expanded.argc;i++) {
        size_t n=length(expanded.argv[i])+1;
        if(n>MAX_VAR_VAL) {puts_err("for: value exceeds 255 bytes\n");return false;}
        if(n>sizeof(frame->values)-used) {puts_err("for: expanded list exceeds 4096 bytes\n");return false;}
        frame->argv[frame->count++]=frame->values+used;
        for(size_t j=0;j<n;j++) frame->values[used++]=expanded.argv[i][j];
    }
    return true;
}
int control_execute(int status,int (*simple)(const char *,int),volatile unsigned *interrupted) {
    if(!ready) return 2;
    if(!running) {depth=control_stop=pc=0;running=true;}
    while(pc<ncode) {
        if(interrupted && *interrupted) {status=128+(int)*interrupted;break;}
        instruction *op=&code[pc++];
        if(op->op==SIMPLE) {
            if(!copy_span(op->start,op->start+op->length,false)) {status=2;break;}
            status=simple(command,status);
            if(control_stop==CONTROL_SOURCE_PENDING) {
                pc--;control_stop=0;return CONTROL_YIELD;
            }
            if(control_stop==CONTROL_RETURN_PENDING) break;
            if(control_stop) {
                unsigned target=depth-levels;
                pc=(unsigned)(again_requested ? frames[target].next : frames[target].end+1);
                depth=target+(again_requested ? 1u : 0u);status=0;
                if(again_requested) frames[target].result=0;
                control_stop=0;
            }
        } else if(op->op==JUMP) pc=(unsigned)op->target;
        else if(op->op==BRANCH) {if(status) pc=(unsigned)op->target;}
        else if(op->op==ZERO) status=0;
        else if(op->op==WHILE_ENTER || op->op==FOR_ENTER) {
            if(depth==CONTROL_MAX_DEPTH) {status=2;break;}
            loop_frame *f=&frames[depth++];f->end=op->target;f->next=op->next;f->result=0;
            if(op->op==FOR_ENTER) {
                for(unsigned i=0;i<MAX_VAR_NAME;i++) f->name[i]=op->name[i];
                if(!for_values(op,f,status)) {status=1;break;}
            }
        } else if(op->op==FOR_NEXT) {
            loop_frame *f=&frames[depth-1];
            if(f->index==f->count) pc=(unsigned)op->target;
            else if(vars_set(f->name,f->argv[f->index++],false)) {status=1;break;}
        } else if(op->op==SAVE_RESULT) frames[depth-1].result=status;
        else if(op->op==LOOP_EXIT) status=frames[--depth].result;
    }
    running=false;depth=control_stop=0;return status;
}

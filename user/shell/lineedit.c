#include "lineedit.h"
static char history_data[HISTORY_BYTES];
static uint32_t history_offsets[HISTORY_COUNT];
static size_t history_used, history_entries;
static size_t len(const char *s) { size_t n=0; while(s[n]) n++; return n; }
static void copy(char *d,const char *s) { do { *d++=*s; } while(*s++); }
static bool same(const char *a,const char *b) { while(*a && *a==*b) { a++; b++; } return *a==*b; }
size_t history_count(void) { return history_entries; }
const char *history_get(size_t i) { return i<history_entries ? history_data+history_offsets[i] : ""; }
void history_clear(void) { history_used=history_entries=0; }
void history_add(const char *s) {
    size_t n=len(s)+1;
    if(n<=1 || n>LINE_CAP || (history_entries && same(s,history_get(history_entries-1)))) return;
    while(history_entries && (history_entries==HISTORY_COUNT || history_used+n>HISTORY_BYTES)) {
        size_t remove=len(history_data)+1;
        for(size_t i=remove;i<history_used;i++) history_data[i-remove]=history_data[i];
        history_used-=remove; history_entries--;
        for(size_t i=0;i<history_entries;i++) history_offsets[i]=history_offsets[i+1]-(uint32_t)remove;
    }
    history_offsets[history_entries++]=(uint32_t)history_used;
    copy(history_data+history_used,s); history_used+=n;
}
void lineedit_init(line_editor_t *e) {
    /* Explicit zeroing avoids a hosted memset dependency. */
    unsigned char *p=(unsigned char *)e;
    for(size_t i=0;i<sizeof(*e);i++) p[i]=0;
    e->history_pos=history_entries;
}
static void load(line_editor_t *e,const char *s) { copy(e->text,s); e->len=len(s); e->cursor=e->len; e->view=0; }
static void erase(line_editor_t *e,size_t from,size_t to,bool save) {
    if(save) { for(size_t i=from;i<to;i++) e->kill[i-from]=e->text[i]; e->kill[to-from]=0; }
    for(size_t i=to;i<=e->len;i++) e->text[from+i-to]=e->text[i];
    e->len-=to-from; e->cursor=from;
}
static void insert(line_editor_t *e,char c) {
    if(e->len==LINE_CAP-1) { e->blocked=true; return; }
    for(size_t i=e->len+1;i>e->cursor;i--) e->text[i]=e->text[i-1];
    e->text[e->cursor++]=c; e->len++;
}
static bool contains(const char *s,const char *q) {
    if(!*q) return true;
    for(;*s;s++) { size_t i=0; while(q[i] && s[i]==q[i]) i++; if(!q[i]) return true; }
    return false;
}
static void search(line_editor_t *e,bool next) {
    size_t start=next?e->search_pos:history_entries;
    e->search_failed=true;
    while(start) { start--; if(contains(history_get(start),e->query)) { load(e,history_get(start)); e->search_pos=start; e->search_failed=false; return; } }
}
static void cancel_search(line_editor_t *e) { load(e,e->saved); e->cursor=e->saved_cursor; e->search=false; }
enum edit_result lineedit_timeout(line_editor_t *e) {
    e->escape_len=0;
    if(e->search) cancel_search(e);
    return EDIT_CHANGED;
}
void lineedit_lost(line_editor_t *e) { e->blocked=true; e->escape_len=0; e->paste=false; }
static void history_move(line_editor_t *e,bool up) {
    if(e->history_pos>history_entries) e->history_pos=history_entries;
    if(up && e->history_pos) {
        if(e->history_pos==history_entries) { copy(e->draft,e->text); e->draft_cursor=e->cursor; }
        load(e,history_get(--e->history_pos));
    } else if(!up && e->history_pos<history_entries) {
        e->history_pos++;
        if(e->history_pos==history_entries) { load(e,e->draft); e->cursor=e->draft_cursor; }
        else load(e,history_get(e->history_pos));
    }
}
static enum edit_result key(line_editor_t *e,char k) {
    if(e->search) cancel_search(e);
    switch(k) {
        case 'A': history_move(e,true); break;
        case 'B': history_move(e,false); break;
        case 'C': if(e->cursor<e->len) e->cursor++; break;
        case 'D': if(e->cursor) e->cursor--; break;
        case 'H': e->cursor=0; break;
        case 'F': e->cursor=e->len; break;
        case 'X': if(e->cursor<e->len) erase(e,e->cursor,e->cursor+1,false); break;
        default: break;
    }
    return EDIT_CHANGED;
}
enum edit_result lineedit_byte(line_editor_t *e,unsigned char c) {
    /* Cancellation must also escape a truncated/malformed bracketed paste. */
    if(c==3) return EDIT_CANCEL;
    if(e->escape_len) {
        if(e->escape_len==sizeof(e->escape)-1) { e->escape_len=0; return EDIT_NONE; }
        e->escape[e->escape_len++]=(char)c; e->escape[e->escape_len]=0;
        if(e->escape_len==2 && (c=='[' || c=='O')) return EDIT_NONE;
        if(e->escape_len==2 || (c>=0x40 && c<=0x7e)) {
            e->escape_len=0;
            if(same(e->escape,"\033[200~")) { e->paste=true; return EDIT_NONE; }
            if(same(e->escape,"\033[201~")) { e->paste=false; e->review=true; return EDIT_CHANGED; }
            if(e->paste) return EDIT_NONE;
            if(same(e->escape,"\033[3~")) return key(e,'X');
            if(same(e->escape,"\033[1~") || same(e->escape,"\033[7~")) return key(e,'H');
            if(same(e->escape,"\033[4~") || same(e->escape,"\033[8~")) return key(e,'F');
            if((e->escape[1]=='[' || e->escape[1]=='O') && e->escape[3]==0) return key(e,(char)c);
            if(e->search) cancel_search(e);
            return EDIT_CHANGED;
        }
        return EDIT_NONE;
    }
    if(c==27) { e->escape[0]=27; e->escape[1]=0; e->escape_len=1; return EDIT_NONE; }
    if(e->paste) {
        if(c=='\n' || c=='\r' || c=='\t') c=' ';
        if(c>=32 && c<127) insert(e,(char)c);
        return EDIT_CHANGED;
    }
    if(e->search) {
        if(c==7) { cancel_search(e); return EDIT_CHANGED; }
        if(c=='\n' || c=='\r') { e->search=false; return EDIT_CHANGED; }
        if(c==18) search(e,true);
        else if(c==8 || c==127) { if(e->query_len) e->query[--e->query_len]=0; search(e,false); }
        else if(c>=32 && c<127 && e->query_len<LINE_CAP-1) { e->query[e->query_len++]=(char)c; e->query[e->query_len]=0; search(e,false); }
        return EDIT_CHANGED;
    }
    switch(c) {
        case '\r': case '\n':
            if(e->blocked) return EDIT_CHANGED;
            if(e->review) { e->review=false; return EDIT_CHANGED; }
            return EDIT_ACCEPT;
        case '\t': return EDIT_COMPLETE;
        case 1: return key(e,'H');
        case 5: return key(e,'F');
        case 4: if(!e->len) return EDIT_EOF; return key(e,'X');
        case 8: case 127: if(e->cursor) erase(e,e->cursor-1,e->cursor,false); break;
        case 21: erase(e,0,e->cursor,true); break;
        case 11: erase(e,e->cursor,e->len,true); break;
        case 23: {
            size_t from=e->cursor;
            while(from && e->text[from-1]==' ') from--;
            while(from && e->text[from-1]!=' ') from--;
            erase(e,from,e->cursor,true); break;
        }
        case 25: for(size_t i=0;e->kill[i];i++) insert(e,e->kill[i]); break;
        case 12: return EDIT_CLEAR;
        case 18:
            copy(e->saved,e->text); e->saved_cursor=e->cursor;
            e->query[0]=0; e->query_len=0; e->search=true; e->search_pos=history_entries;
            search(e,false); break;
        default: if(c>=32 && c<127) insert(e,(char)c); else return EDIT_NONE;
    }
    return EDIT_CHANGED;
}

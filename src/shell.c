/*
** 2001 September 15
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains code to implement the "sqlite" command line
** utility for accessing SQLite databases.
**
** $Id: shell.c,v 1.57 2002/05/21 13:02:24 drh Exp $
*/
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "sqlite.h"
#include <ctype.h>

// 윈도우즈가 아닌 운영 체제에서만 포함되는 헤더 파일들을 지정
// <signal.h>: 시그널 처리 함수들
// <pwd.h>: 사용자 정보 조회 함수들 (예: 홈 디렉토리 찾기)
// <unistd.h>: POSIX API 함수들 (예: getuid, isatty 등)
// <sys/types.h>: 다양한 데이터 타입 정의
#if !defined(_WIN32) && !defined(WIN32)
# include <signal.h>
# include <pwd.h>
# include <unistd.h>
# include <sys/types.h>
#endif

// Readline 라이브러리 사용 여부에 따른 조건부 컴파일:
// HAVE_READLINE이 정의되고 값이 1인 경우, Readline 라이브러리를 포함하여 명령어 편집과 히스토리 기능을 사용합니다.
// 그렇지 않은 경우, readline과 관련된 함수들을 자체 구현한 getline_internal으로 대체하고, 히스토리 관련 함수들을 빈 매크로로 정의합니다. 
// 이는 Readline 라이브러리가 없는 환경에서의 호환성을 유지하기 위함입니다.
#if defined(HAVE_READLINE) && HAVE_READLINE==1
# include <readline/readline.h>
# include <readline/history.h>
#else
# define readline(p) getline_internal(p,stdin)
# define add_history(X)
# define read_history(X)
# define write_history(X)
# define stifle_history(X)
#endif

// 현재 열려 있는 SQLite 데이터베이스를 가리키는 포인터입니다.
// static으로 선언되어 파일 내에서만 접근 가능하며, 시그널 핸들러에서도 접근할 수 있도록 합니다.
static sqlite *db = 0;

// 사용자가 Ctrl-C를 눌러 인터럽트를 발생시켰는지 여부를 나타내는 플래그입니다.
static int seenInterrupt = 0;

// 프로그램의 이름을 저장하는 변수로, main() 함수에서 설정되며 주로 오류 메시지에 사용됩니다.
static char *Argv0;

// 커맨드 라인 셸에서 사용자에게 표시되는 프롬프트 문자열을 저장합니다.
// mainPrompt: 기본 프롬프트 ("sqlite> ").
// continuePrompt: SQL 명령어가 여러 줄에 걸쳐 입력될 때 사용하는 연속 프롬프트 (" ...> ").
static char mainPrompt[20];     /* First line prompt. default: "sqlite> "*/
static char continuePrompt[20]; /* Continuation prompt. default: "   ...> " */

// 목적: 표준 입력(stdin)에서 한 줄의 텍스트를 읽어 메모리에 저장하고, 그 포인터를 반환합니다. 
// EOF나 malloc() 실패 시 NULL을 반환합니다.
// 매개변수:
// char *zPrompt: 프롬프트 문자열. NULL이거나 빈 문자열인 경우 프롬프트를 표시하지 않습니다.
// FILE *in: 입력을 받을 파일 스트림.
static char *getline_internal(char *zPrompt, FILE *in){
  char *zLine;
  int nLine;
  int n;
  int eol;

  // 프롬프트가 지정되면 출력하고 stdout을 플러시합니다.
  if( zPrompt && *zPrompt ){
    printf("%s",zPrompt);
    fflush(stdout);
  }
  // 초기 버퍼 크기를 100바이트로 설정하고 메모리를 할당합니다.
  nLine = 100;
  zLine = malloc( nLine );
  if( zLine==0 ) return 0;
  n = 0;
  eol = 0;
  // 루프를 돌며 입력을 읽습니다
  while( !eol ){
    if( n+100>nLine ){
      nLine = nLine*2 + 100;
      // 현재 버퍼 크기를 초과할 경우, 버퍼 크기를 동적으로 늘립니다 (realloc).
      zLine = realloc(zLine, nLine);
      if( zLine==0 ) return 0;
    }
    // fgets로 한 줄을 읽습니다. EOF나 에러 시:
    if( fgets(&zLine[n], nLine - n, in)==0 ){
      // 아무것도 읽지 않았다면 메모리를 해제하고 NULL 반환.
      if( n==0 ){
        free(zLine);
        return 0;
      }
      // 일부를 읽었다면 문자열을 종료하고 루프를 종료.
      // 읽은 문자열에서 줄 끝(\n)을 제거하고 eol 플래그를 설정합니다.
      zLine[n] = 0;
      eol = 1;
      break;
    }
    while( zLine[n] ){ n++; }

    // 읽은 문자열에서 줄 끝 제거
    if( n>0 && zLine[n-1]=='\n' ){
      n--;
      zLine[n] = 0;
      eol = 1;
    }
  }
  // 최종적으로 버퍼를 정확한 크기로 줄이고 포인터를 반환합니다.
  zLine = realloc( zLine, n+1 );
  return zLine;
}

// 목적: 한 줄의 입력을 읽어 반환합니다. 
// 입력이 터미널에서 오는 경우 Readline을 사용해 명령어 편집 기능을 제공합니다. 
// 그렇지 않으면 getline_internal을 사용합니다.
// const char *zPrior: 이전에 읽은 텍스트. 연속된 입력(예: 여러 줄에 걸친 SQL 명령어) 시 연속 프롬프트를 표시하기 위해 사용됩니다.
// FILE *in: 입력을 받을 파일 스트림.
static char *one_input_line(const char *zPrior, FILE *in){
  char *zPrompt;
  char *zResult;
  // 입력이 파일 스트림(in)에서 오는 경우, getline_internal을 사용해 한 줄을 읽고 반환합니다.
  if( in!=0 ){
    return getline_internal(0, in);
  }
  // 터미널에서 오는 경우:
  // zPrior가 비어 있지 않으면 continuePrompt를, 그렇지 않으면 mainPrompt를 프롬프트로 설정합니다.
  if( zPrior && zPrior[0] ){
    zPrompt = continuePrompt;
  }else{
    zPrompt = mainPrompt;
  }
  // readline을 사용해 입력을 받고, 히스토리에 추가합니다.
  zResult = readline(zPrompt);
  if( zResult ) add_history(zResult);
  // 결과를 반환합니다.
  return zResult;
}

// struct previous_mode_data:
// 이전 모드 정보를 저장하는 구조체로, .explain ON 명령어 이전의 상태를 저장합니다.
// valid: 유효한 데이터인지 여부.
// mode: 출력 모드.
// showHeader: 헤더 표시 여부.
// colWidth[100]: 열 너비.
struct previous_mode_data {
  int valid;
  int mode;
  int showHeader;
  int colWidth[100];
};

// struct callback_data:
// SQLite 콜백 함수로 전달되는 구조체로, 상태 및 모드 정보를 저장합니다.
// sqlite *db: 데이터베이스 포인터.
// int echoOn: 명령어 에코 여부.
// int cnt: 현재까지 표시된 레코드 수.
// FILE *out: 결과를 출력할 파일 스트림.
// int mode: 출력 모드 (라인, 컬럼 등).
// int showHeader: 헤더 표시 여부.
// char *zDestTable: MODE_Insert 모드에서 사용되는 대상 테이블 이름.
// char separator[20]: MODE_List 모드에서 사용하는 구분자.
// int colWidth[100]: 요청된 열 너비.
// int actualWidth[100]: 실제 열 너비.
// char nullvalue[20]: NULL값을 대체하는 문자열.
// struct previous_mode_data explainPrev: .explain ON 이전의 모드 정보.
// char outfile[FILENAME_MAX]: 출력 파일 이름.
struct callback_data {
  sqlite *db;            /* The database */
  int echoOn;            /* True to echo input commands */
  int cnt;               /* Number of records displayed so far */
  FILE *out;             /* Write results here */
  int mode;              /* An output mode setting */
  int showHeader;        /* True to show column names in List or Column mode */
  char *zDestTable;      /* Name of destination table when MODE_Insert */
  char separator[20];    /* Separator character for MODE_List */
  int colWidth[100];     /* Requested width of each column when in column mode*/
  int actualWidth[100];  /* Actual width of each column */
  char nullvalue[20];    /* The text to print when a NULL comes back from the database */
  struct previous_mode_data explainPrev;
                         /* Holds the mode information just before .explain ON */
  char outfile[FILENAME_MAX];
                         /* Filename for *out */
};

// MODE_Line: 한 열씩 줄 단위로 출력, 레코드 사이에 빈 줄 추가.
// MODE_Column: 깔끔한 컬럼 형식으로 한 레코드씩 출력.
// MODE_List: 구분자를 사용해 한 줄에 한 레코드씩 출력.
// MODE_Semi: MODE_List와 동일하지만 각 줄 끝에 ; 추가.
// MODE_Html: XHTML 테이블 형식으로 출력.
// MODE_Insert: SQL INSERT 문으로 출력.
// MODE_NUM_OF: 모드의 총 개수.
#define MODE_Line     0  /* One column per line.  Blank line between records */
#define MODE_Column   1  /* One record per line in neat columns */
#define MODE_List     2  /* One record per line with a separator */
#define MODE_Semi     3  /* Same as MODE_List but append ";" to each line */
#define MODE_Html     4  /* Generate an XHTML table */
#define MODE_Insert   5  /* Generate SQL "insert" statements */
#define MODE_NUM_OF   6

// 모드 설명 배열:
// 각 모드에 대한 문자열 설명을 저장합니다.
char *modeDescr[MODE_NUM_OF] = {
  "line",
  "column",
  "list",
  "semi",
  "html",
  "insert"
};

// 배열의 요소 개수를 계산하는 매크로입니다.
#define ArraySize(X)  (sizeof(X)/sizeof(X[0]))

// 주어진 문자열이 숫자인지 여부를 판단합니다.
static int is_numeric(const char *z){
  int seen_digit = 0;
  // 문자열이 - 또는 +로 시작하면 건너뜁니다.
  if( *z=='-' || *z=='+' ){
    z++;
  }
  // 숫자(0-9)가 연속적으로 있는지 확인하고, 최소 하나의 숫자를 봤는지 기록합니다.
  while( isdigit(*z) ){
    seen_digit = 1;
    z++;
  }
  // 소수점(.)이 있으면, 그 이후에도 숫자가 있는지 확인합니다.
  if( seen_digit && *z=='.' ){
    z++;
    while( isdigit(*z) ){ z++; }
  }
  // 지수 표기법(e 또는 E)이 있으면, 그 다음에 숫자 또는 -, +와 숫자가 오는지 확인합니다.
  if( seen_digit && (*z=='e' || *z=='E')
   && (isdigit(z[1]) || ((z[1]=='-' || z[1]=='+') && isdigit(z[2])))
  ){
    z+=2;
    while( isdigit(*z) ){ z++; }
  }
  // 모든 문자 처리가 끝나고, 유효한 숫자를 봤으며 문자열이 끝났다면 TRUE를 반환합니다.
  return seen_digit && *z==0;
}

// 목적: 주어진 문자열을 SQL의 인용 규칙에 따라 인용된 문자열로 출력합니다.
// 매개변수:
// - FILE *out: 출력할 파일 스트림.
// - const char *z: 인용할 문자열.
static void output_quoted_string(FILE *out, const char *z){
  int i;
  int nSingle = 0;
  // 문자열 내에 단일 인용부호(')의 개수를 셉니다.
  for(i=0; z[i]; i++){
    if( z[i]=='\'' ) nSingle++;
  }
  // 단일 인용부호가 없으면, 단순히 작은 따옴표로 감싸서 출력합니다.
  if( nSingle==0 ){
    fprintf(out,"'%s'",z);
  }else{ // 단일 인용부호가 있으면:
    fprintf(out,"'");
    while( *z ){
      // 첫 번째 작은 따옴표 앞까지 출력한 후, 작은 따옴표를 두 번('') 출력하여 이스케이프합니다.
      for(i=0; z[i] && z[i]!='\''; i++){}
      if( i==0 ){
        fprintf(out,"''");
        z++;
      }else if( z[i]=='\'' ){
        fprintf(out,"%.*s''",i,z);
        z += i+1;
      }else{
        fprintf(out,"%s",z);
        break;
      }
    }
    // 전체 문자열을 작은 따옴표로 감싸서 출력합니다.
    fprintf(out,"'");
  }
}

// 목적: 주어진 문자열에서 HTML 특수 문자인 <와 &를 HTML 엔티티(&lt;, &amp;)로 변환하여 출력합니다.
// 매개변수
// - FILE *out: 출력할 파일 스트림.
// - const char *z: 변환할 문자열.
static void output_html_string(FILE *out, const char *z){
  int i;
  while( *z ){
    // 문자열을 순회하며 < 또는 & 문자가 나타날 때까지 출력합니다.
    // 특수 문자가 나오면, 해당 문자를 대응하는 HTML 엔티티로 출력합니다.
    // 변환된 부분 이후의 문자열을 계속 처리합니다.
    for(i=0; z[i] && z[i]!='<' && z[i]!='&'; i++){}
    if( i>0 ){
      fprintf(out,"%.*s",i,z);
    }
    if( z[i]=='<' ){
      fprintf(out,"&lt;");
    }else if( z[i]=='&' ){
      fprintf(out,"&amp;");
    }else{
      break;
    }
    z += i + 1;
  }
}

// 목적: 사용자가 Ctrl-C를 눌렀을 때 실행되는 시그널 핸들러입니다.
// 동작:
// seenInterrupt 플래그를 설정하여 인터럽트가 발생했음을 기록합니다.
// 열려 있는 데이터베이스가 있으면, SQLite 라이브러리의 sqlite_interrupt 함수를 호출하여 현재 진행 중인 쿼리를 중단합니다.
static void interrupt_handler(int NotUsed){
  seenInterrupt = 1;
  if( db ) sqlite_interrupt(db);
}

// 목적: SQLite 쿼리 결과의 각 행에 대해 호출되는 콜백 함수로, 현재 설정된 출력 모드에 따라 결과를 포맷하여 출력합니다.
static int callback(void *pArg, int nArg, char **azArg, char **azCol){
  int i;
  struct callback_data *p = (struct callback_data*)pArg;
  switch( p->mode ){
    case MODE_Line: {
      // 각 열을 "컬럼 이름 = 값" 형식으로 출력합니다.
      // 레코드 간에 빈 줄을 삽입합니다.
      int w = 5;
      if( azArg==0 ) break;
      for(i=0; i<nArg; i++){
        int len = strlen(azCol[i]);
        if( len>w ) w = len;
      }
      if( p->cnt++>0 ) fprintf(p->out,"\n");
      for(i=0; i<nArg; i++){
        fprintf(p->out,"%*s = %s\n", w, azCol[i], azArg[i] ? azArg[i] : p->nullvalue);
      }
      break;
    }
    case MODE_Column: {
      // 첫 번째 레코드일 경우, 헤더를 출력하고 컬럼 너비를 설정합니다.
      // 이후 레코드들을 컬럼 형식으로 출력합니다.
      if( p->cnt++==0 ){
        for(i=0; i<nArg; i++){
          int w, n;
          if( i<ArraySize(p->colWidth) ){
             w = p->colWidth[i];
          }else{
             w = 0;
          }
          if( w<=0 ){
            w = strlen(azCol[i] ? azCol[i] : "");
            if( w<10 ) w = 10;
            n = strlen(azArg && azArg[i] ? azArg[i] : p->nullvalue);
            if( w<n ) w = n;
          }
          if( i<ArraySize(p->actualWidth) ){
            p->actualWidth[i] = w;
          }
          if( p->showHeader ){
            fprintf(p->out,"%-*.*s%s",w,w,azCol[i], i==nArg-1 ? "\n": "  ");
          }
        }
        if( p->showHeader ){
          for(i=0; i<nArg; i++){
            int w;
            if( i<ArraySize(p->actualWidth) ){
               w = p->actualWidth[i];
            }else{
               w = 10;
            }
            fprintf(p->out,"%-*.*s%s",w,w,"-----------------------------------"
                   "----------------------------------------------------------",
                    i==nArg-1 ? "\n": "  ");
          }
        }
      }
      if( azArg==0 ) break;
      for(i=0; i<nArg; i++){
        int w;
        if( i<ArraySize(p->actualWidth) ){
           w = p->actualWidth[i];
        }else{
           w = 10;
        }
        fprintf(p->out,"%-*.*s%s",w,w,
            azArg[i] ? azArg[i] : p->nullvalue, i==nArg-1 ? "\n": "  ");
      }
      break;
    }
    case MODE_Semi:
    case MODE_List: {
      // 각 레코드를 구분자(p->separator)를 사용해 한 줄에 출력합니다.
      // MODE_Semi에서는 각 줄 끝에 ;를 추가합니다.
      if( p->cnt++==0 && p->showHeader ){
        for(i=0; i<nArg; i++){
          fprintf(p->out,"%s%s",azCol[i], i==nArg-1 ? "\n" : p->separator);
        }
      }
      if( azArg==0 ) break;
      for(i=0; i<nArg; i++){
        char *z = azArg[i];
        if( z==0 ) z = p->nullvalue;
        fprintf(p->out, "%s", z);
        if( i<nArg-1 ){
          fprintf(p->out, "%s", p->separator);
        }else if( p->mode==MODE_Semi ){
          fprintf(p->out, ";\n");
        }else{
          fprintf(p->out, "\n");
        }
      }
      break;
    }
    case MODE_Html: {
      // 결과를 XHTML 테이블 형식으로 출력합니다.
      if( p->cnt++==0 && p->showHeader ){
        fprintf(p->out,"<TR>");
        for(i=0; i<nArg; i++){
          fprintf(p->out,"<TH>%s</TH>",azCol[i]);
        }
        fprintf(p->out,"</TR>\n");
      }
      if( azArg==0 ) break;
      fprintf(p->out,"<TR>");
      for(i=0; i<nArg; i++){
        fprintf(p->out,"<TD>");
        output_html_string(p->out, azArg[i] ? azArg[i] : p->nullvalue);
        fprintf(p->out,"</TD>\n");
      }
      fprintf(p->out,"</TD></TR>\n");
      break;
    }
    case MODE_Insert: {
      // 각 레코드를 SQL INSERT 문으로 변환해 출력합니다.
      if( azArg==0 ) break;
      fprintf(p->out,"INSERT INTO %s VALUES(",p->zDestTable);
      for(i=0; i<nArg; i++){
        char *zSep = i>0 ? ",": "";
        if( azArg[i]==0 ){
          fprintf(p->out,"%sNULL",zSep);
        }else if( is_numeric(azArg[i]) ){
          fprintf(p->out,"%s%s",zSep, azArg[i]);
        }else{
          if( zSep[0] ) fprintf(p->out,"%s",zSep);
          output_quoted_string(p->out, azArg[i]);
        }
      }
      fprintf(p->out,");\n");
      break;
    }
  }
  return 0;
}

// 목적: callback_data 구조체의 zDestTable 필드를 주어진 테이블 이름으로 설정하며, 테이블 이름에 포함된 작은 따옴표를 이스케이프합니다.
// 매개변수:
// - struct callback_data *p: 콜백 데이터 구조체 포인터.
// - const char *zName: 설정할 테이블 이름.
static void set_table_name(struct callback_data *p, const char *zName){
  int i, n;
  int needQuote;
  char *z;

  // 기존에 설정된 테이블 이름이 있다면 메모리를 해제하고 초기화합니다.
  if( p->zDestTable ){
    free(p->zDestTable);
    p->zDestTable = 0;
  }
  // 테이블 이름이 NULL이면 함수를 종료합니다.
  if( zName==0 ) return;
  // 테이블 이름에 숫자나 _가 아닌 문자가 있는지 검사하여 인용부호가 필요한지 결정합니다.
  needQuote = !isalpha(*zName) && *zName!='_';
  for(i=n=0; zName[i]; i++, n++){
    if( !isalnum(zName[i]) && zName[i]!='_' ){
      needQuote = 1;
      // 테이블 이름에 작은 따옴표가 포함되어 있으면 이스케이프 처리를 위해 n을 증가시킵니다.
      if( zName[i]=='\'' ) n++;
    }
  }
  if( needQuote ) n += 2;
  z = p->zDestTable = malloc( n+1 );
  if( z==0 ){
    fprintf(stderr,"Out of memory!\n");
    exit(1);
  }
  n = 0;
  // 필요에 따라 작은 따옴표로 테이블 이름을 감싸고, 내부의 작은 따옴표를 두 번('')으로 변환합니다.
  if( needQuote ) z[n++] = '\'';
  // 최종적으로 zDestTable에 할당합니다.
  for(i=0; zName[i]; i++){
    z[n++] = zName[i];
    if( zName[i]=='\'' ) z[n++] = '\'';
  }
  if( needQuote ) z[n++] = '\'';
  z[n] = 0;
}

// 목적: 데이터베이스 덤프를 수행하는 동안 각 테이블이나 인덱스에 대한 SQL CREATE 문을 출력하고, 테이블의 데이터를 INSERT 문으로 출력합니다.
// 매개변수:
// void *pArg: 콜백 데이터 구조체 포인터.
// int nArg: 열의 개수 (기대: 3).
// char **azArg: 열의 값들 (테이블 이름, 타입, SQL).
// char **azCol: 열의 이름들.
static int dump_callback(void *pArg, int nArg, char **azArg, char **azCol){
  struct callback_data *p = (struct callback_data *)pArg;
  // 열의 개수가 3이 아니면 에러를 반환합니다.
  if( nArg!=3 ) return 1; 
  fprintf(p->out, "%s;\n", azArg[2]);
  // 테이블 타입이 "table"인 경우:
  // callback_data 구조체 d2를 초기화하고 MODE_Insert로 설정합니다.
  // SQL CREATE 문(azArg[2])을 출력합니다.
  if( strcmp(azArg[1],"table")==0 ){
    struct callback_data d2;
    d2 = *p;
    d2.mode = MODE_Insert;
    d2.zDestTable = 0;
    set_table_name(&d2, azArg[0]);
    sqlite_exec_printf(p->db,
       "SELECT * FROM '%q'",
       callback, &d2, 0, azArg[0]
    );
    // 테이블 이름을 설정하고, 해당 테이블의 모든 데이터를 INSERT 문으로 덤프합니다.
    set_table_name(&d2, 0);
  }
  return 0;
}

// 목적: 사용자에게 제공되는 도움말 메시지로, 지원되는 셸 명령어들을 설명합니다.
// 내용:
// .dump: 데이터베이스를 텍스트 형식으로 덤프합니다.
// .echo: 명령어 에코를 켜거나 끕니다.
// .exit / .quit: 프로그램을 종료합니다.
// .explain: EXPLAIN 모드 출력을 켜거나 끕니다.
// .header: 헤더 표시를 켜거나 끕니다.
// .help: 도움말 메시지를 표시합니다.
// .indices: 특정 테이블의 모든 인덱스 이름을 표시합니다.
// .mode: 출력 모드를 설정합니다 (line, column, insert, list, html 등).
// .nullvalue: NULL 값 대신 표시할 문자열을 설정합니다.
// .output: 출력 대상 파일을 설정합니다.
// .prompt: 프롬프트 문자열을 설정합니다.
// .read: 파일의 SQL 명령어를 실행합니다.
// .reindex: 인덱스를 재구성합니다.
// .schema: 테이블의 CREATE 문을 표시합니다.
// .separator: list 모드에서 사용할 구분자를 설정합니다.
// .show: 현재 설정된 다양한 옵션값을 표시합니다.
// .tables: 패턴에 맞는 테이블 이름을 나열합니다.
// .timeout: 테이블 잠금 시도 시간을 설정합니다.
// .width: column 모드에서의 열 너비를 설정합니다.
static char zHelp[] =
  ".dump ?TABLE? ...      Dump the database in an text format\n"
  ".echo ON|OFF           Turn command echo on or off\n"
  ".exit                  Exit this program\n"
  ".explain ON|OFF        Turn output mode suitable for EXPLAIN on or off.\n"
  ".header(s) ON|OFF      Turn display of headers on or off\n"
  ".help                  Show this message\n"
  ".indices TABLE         Show names of all indices on TABLE\n"
  ".mode MODE             Set mode to one of \"line(s)\", \"column(s)\", \n"
  "                       \"insert\", \"list\", or \"html\"\n"
  ".mode insert TABLE     Generate SQL insert statements for TABLE\n"
  ".nullvalue STRING      Print STRING instead of nothing for NULL data\n"
  ".output FILENAME       Send output to FILENAME\n"
  ".output stdout         Send output to the screen\n"
  ".prompt MAIN CONTINUE  Replace the standard prompts\n"
  ".quit                  Exit this program\n"
  ".read FILENAME         Execute SQL in FILENAME\n"
  ".reindex ?TABLE?       Rebuild indices\n"
/*  ".rename OLD NEW        Change the name of a table or index\n" */
  ".schema ?TABLE?        Show the CREATE statements\n"
  ".separator STRING      Change separator string for \"list\" mode\n"
  ".show                  Show the current values for various settings\n"
  ".tables ?PATTERN?      List names of tables matching a pattern\n"
  ".timeout MS            Try opening locked tables for MS milliseconds\n"
  ".width NUM NUM ...     Set column widths for \"column\" mode\n"
;

/* Forward reference */
static void process_input(struct callback_data *p, FILE *in);

// 목적: 입력된 메타 명령어(.으로 시작하는 명령어)를 처리합니다.
// 매개변수:
//char *zLine: 입력된 명령어 문자열.
// sqlite *db: 데이터베이스 포인터.
// struct callback_data *p: 콜백 데이터 구조체 포인터.
// 지원되는 명령어 및 처리:
// .dump: 데이터베이스를 덤프합니다.
// .echo: 명령어 에코를 켜거나 끕니다.
// .exit: 프로그램을 종료합니다.
// .explain: EXPLAIN 모드를 켜거나 끕니다.
// .header(s): 헤더 표시를 켜거나 끕니다.
// .help: 도움말 메시지를 출력합니다.
// .indices: 특정 테이블의 인덱스 이름을 출력합니다.
// .mode: 출력 모드를 설정합니다.
// .nullvalue: NULL 값 대체 문자열을 설정합니다.
// .output: 출력 대상 파일을 설정합니다.
// .prompt: 프롬프트 문자열을 설정합니다.
// .quit: 프로그램을 종료합니다.
// .read: 파일에서 SQL 명령어를 읽어 실행합니다.
// .reindex: 인덱스를 재구성합니다.
// .schema: 테이블의 CREATE 문을 출력합니다.
// .separator: list 모드의 구분자를 설정합니다.
// .show: 현재 설정된 옵션값을 출력합니다.
// .tables: 테이블 이름을 출력합니다.
// .timeout: 테이블 잠금 시도 시간을 설정합니다.
// .width: column 모드의 열 너비를 설정합니다.
static int do_meta_command(char *zLine, sqlite *db, struct callback_data *p){
  int i = 1;
  int nArg = 0;
  int n, c;
  int rc = 0;
  char *azArg[50];

  /* Parse the input line into tokens.
  */
  while( zLine[i] && nArg<ArraySize(azArg) ){
    // 입력된 명령어를 공백 기준으로 토큰화하여 azArg 배열에 저장합니다.
    while( isspace(zLine[i]) ){ i++; }
    // 작은 따옴표(')나 큰 따옴표(")로 묶인 문자열은 하나의 인수로 처리합니다.
    if( zLine[i]=='\'' || zLine[i]=='"' ){
      int delim = zLine[i++];
      azArg[nArg++] = &zLine[i];
      while( zLine[i] && zLine[i]!=delim ){ i++; }
      if( zLine[i]==delim ){
        zLine[i++] = 0;
      }
    }else{
      azArg[nArg++] = &zLine[i];
      while( zLine[i] && !isspace(zLine[i]) ){ i++; }
      if( zLine[i] ) zLine[i++] = 0;
    }
  }

  /* Process the input line.
  */
  if( nArg==0 ) return rc;
  n = strlen(azArg[0]);
  c = azArg[0][0];
  if( c=='d' && strncmp(azArg[0], "dump", n)==0 ){
    char *zErrMsg = 0;
    fprintf(p->out, "BEGIN TRANSACTION;\n");
    if( nArg==1 ){
      sqlite_exec(db,
        "SELECT name, type, sql FROM sqlite_master "
        "WHERE type!='meta' AND sql NOT NULL "
        "ORDER BY substr(type,2,1), name",
        dump_callback, p, &zErrMsg
      );
    }else{
      int i;
      for(i=1; i<nArg && zErrMsg==0; i++){
        sqlite_exec_printf(db,
          "SELECT name, type, sql FROM sqlite_master "
          "WHERE tbl_name LIKE '%q' AND type!='meta' AND sql NOT NULL "
          "ORDER BY substr(type,2,1), name",
          dump_callback, p, &zErrMsg, azArg[i]
        );
      }
    }
    if( zErrMsg ){
      fprintf(stderr,"Error: %s\n", zErrMsg);
      free(zErrMsg);
    }else{
      fprintf(p->out, "COMMIT;\n");
    }
  }else

  if( c=='e' && strncmp(azArg[0], "echo", n)==0 && nArg>1 ){
    int j;
    char *z = azArg[1];
    int val = atoi(azArg[1]);
    for(j=0; z[j]; j++){
      if( isupper(z[j]) ) z[j] = tolower(z[j]);
    }
    if( strcmp(z,"on")==0 ){
      val = 1;
    }else if( strcmp(z,"yes")==0 ){
      val = 1;
    }
    p->echoOn = val;
  }else

  if( c=='e' && strncmp(azArg[0], "exit", n)==0 ){
    rc = 1;
  }else

  if( c=='e' && strncmp(azArg[0], "explain", n)==0 ){
    int j;
    char *z = nArg>=2 ? azArg[1] : "1";
    int val = atoi(z);
    for(j=0; z[j]; j++){
      if( isupper(z[j]) ) z[j] = tolower(z[j]);
    }
    if( strcmp(z,"on")==0 ){
      val = 1;
    }else if( strcmp(z,"yes")==0 ){
      val = 1;
    }
    if(val == 1) {
      if(!p->explainPrev.valid) {
        p->explainPrev.valid = 1;
        p->explainPrev.mode = p->mode;
        p->explainPrev.showHeader = p->showHeader;
        memcpy(p->explainPrev.colWidth,p->colWidth,sizeof(p->colWidth));
      }
      /* We could put this code under the !p->explainValid
      ** condition so that it does not execute if we are already in
      ** explain mode. However, always executing it allows us an easy
      ** was to reset to explain mode in case the user previously
      ** did an .explain followed by a .width, .mode or .header
      ** command.
      */
      p->mode = MODE_Column;
      p->showHeader = 1;
      memset(p->colWidth,0,ArraySize(p->colWidth));
      p->colWidth[0] = 4;
      p->colWidth[1] = 12;
      p->colWidth[2] = 10;
      p->colWidth[3] = 10;
      p->colWidth[4] = 35;
    }else if (p->explainPrev.valid) {
      p->explainPrev.valid = 0;
      p->mode = p->explainPrev.mode;
      p->showHeader = p->explainPrev.showHeader;
      memcpy(p->colWidth,p->explainPrev.colWidth,sizeof(p->colWidth));
    }
  }else

  if( c=='h' && (strncmp(azArg[0], "header", n)==0
                 ||
                 strncmp(azArg[0], "headers", n)==0 )&& nArg>1 ){
    int j;
    char *z = azArg[1];
    int val = atoi(azArg[1]);
    for(j=0; z[j]; j++){
      if( isupper(z[j]) ) z[j] = tolower(z[j]);
    }
    if( strcmp(z,"on")==0 ){
      val = 1;
    }else if( strcmp(z,"yes")==0 ){
      val = 1;
    }
    p->showHeader = val;
  }else

  if( c=='h' && strncmp(azArg[0], "help", n)==0 ){
    fprintf(stderr,zHelp);
  }else

  if( c=='i' && strncmp(azArg[0], "indices", n)==0 && nArg>1 ){
    struct callback_data data;
    char *zErrMsg = 0;
    memcpy(&data, p, sizeof(data));
    data.showHeader = 0;
    data.mode = MODE_List;
    sqlite_exec_printf(db,
      "SELECT name FROM sqlite_master "
      "WHERE type='index' AND tbl_name LIKE '%q' "
      "ORDER BY name",
      callback, &data, &zErrMsg, azArg[1]
    );
    if( zErrMsg ){
      fprintf(stderr,"Error: %s\n", zErrMsg);
      free(zErrMsg);
    }
  }else

  if( c=='m' && strncmp(azArg[0], "mode", n)==0 && nArg>=2 ){
    int n2 = strlen(azArg[1]);
    if( strncmp(azArg[1],"line",n2)==0
        ||
        strncmp(azArg[1],"lines",n2)==0 ){
      p->mode = MODE_Line;
    }else if( strncmp(azArg[1],"column",n2)==0
              ||
              strncmp(azArg[1],"columns",n2)==0 ){
      p->mode = MODE_Column;
    }else if( strncmp(azArg[1],"list",n2)==0 ){
      p->mode = MODE_List;
    }else if( strncmp(azArg[1],"html",n2)==0 ){
      p->mode = MODE_Html;
    }else if( strncmp(azArg[1],"insert",n2)==0 ){
      p->mode = MODE_Insert;
      if( nArg>=3 ){
        set_table_name(p, azArg[2]);
      }else{
        set_table_name(p, "table");
      }
    }else {
      fprintf(stderr,"mode should be on of: column html insert line list\n");
    }
  }else

  if( c=='n' && strncmp(azArg[0], "nullvalue", n)==0 && nArg==2 ) {
    sprintf(p->nullvalue, "%.*s", (int)ArraySize(p->nullvalue)-1, azArg[1]);
  }else

  if( c=='o' && strncmp(azArg[0], "output", n)==0 && nArg==2 ){
    if( p->out!=stdout ){
      fclose(p->out);
    }
    if( strcmp(azArg[1],"stdout")==0 ){
      p->out = stdout;
      strcpy(p->outfile,"stdout");
    }else{
      p->out = fopen(azArg[1], "w");
      if( p->out==0 ){
        fprintf(stderr,"can't write to \"%s\"\n", azArg[1]);
        p->out = stdout;
      } else {
         strcpy(p->outfile,azArg[1]);
      }
    }
  }else

  if( c=='p' && strncmp(azArg[0], "prompt", n)==0 && (nArg==2 || nArg==3)){
    if( nArg >= 2) {
      strncpy(mainPrompt,azArg[1],(int)ArraySize(mainPrompt)-1);
    }
    if( nArg >= 3) {
      strncpy(continuePrompt,azArg[2],(int)ArraySize(continuePrompt)-1);
    }
  }else

  if( c=='q' && strncmp(azArg[0], "quit", n)==0 ){
    sqlite_close(db);
    exit(0);
  }else

  if( c=='r' && strncmp(azArg[0], "read", n)==0 && nArg==2 ){
    FILE *alt = fopen(azArg[1], "r");
    if( alt==0 ){
      fprintf(stderr,"can't open \"%s\"\n", azArg[1]);
    }else{
      process_input(p, alt);
      fclose(alt);
    }
  }else

  if( c=='r' && strncmp(azArg[0], "reindex", n)==0 ){
    char **azResult;
    int nRow, rc;
    char *zErrMsg;
    int i;
    char *zSql;
    if( nArg==1 ){
      rc = sqlite_get_table(db,
        "SELECT name, sql FROM sqlite_master "
        "WHERE type='index'",
        &azResult, &nRow, 0, &zErrMsg
      );
    }else{
      rc = sqlite_get_table_printf(db,
        "SELECT name, sql FROM sqlite_master "
        "WHERE type='index' AND tbl_name LIKE '%q'",
        &azResult, &nRow, 0, &zErrMsg, azArg[1]
      );
    }
    for(i=1; rc==SQLITE_OK && i<=nRow; i++){
      extern char *sqlite_mprintf(const char *, ...);
      zSql = sqlite_mprintf(
         "DROP INDEX '%q';\n%s;\nVACUUM '%q';",
         azResult[i*2], azResult[i*2+1], azResult[i*2]);
      if( p->echoOn ) printf("%s\n", zSql);
      rc = sqlite_exec(db, zSql, 0, 0, &zErrMsg);
    }
    sqlite_free_table(azResult);
    if( zErrMsg ){
      fprintf(stderr,"Error: %s\n", zErrMsg);
      free(zErrMsg);
    }
  }else

  if( c=='s' && strncmp(azArg[0], "schema", n)==0 ){
    struct callback_data data;
    char *zErrMsg = 0;
    memcpy(&data, p, sizeof(data));
    data.showHeader = 0;
    data.mode = MODE_Semi;
    if( nArg>1 ){
      extern int sqliteStrICmp(const char*,const char*);
      if( sqliteStrICmp(azArg[1],"sqlite_master")==0 ){
        char *new_argv[2], *new_colv[2];
        new_argv[0] = "CREATE TABLE sqlite_master (\n"
                      "  type text,\n"
                      "  name text,\n"
                      "  tbl_name text,\n"
                      "  rootpage integer,\n"
                      "  sql text\n"
                      ")";
        new_argv[1] = 0;
        new_colv[0] = "sql";
        new_colv[1] = 0;
        callback(&data, 1, new_argv, new_colv);
      }else{
        sqlite_exec_printf(db,
          "SELECT sql FROM sqlite_master "
          "WHERE tbl_name LIKE '%q' AND type!='meta' AND sql NOTNULL "
          "ORDER BY type DESC, name",
          callback, &data, &zErrMsg, azArg[1]);
      }
    }else{
      sqlite_exec(db,
         "SELECT sql FROM sqlite_master "
         "WHERE type!='meta' AND sql NOTNULL "
         "ORDER BY tbl_name, type DESC, name",
         callback, &data, &zErrMsg
      );
    }
    if( zErrMsg ){
      fprintf(stderr,"Error: %s\n", zErrMsg);
      free(zErrMsg);
    }
  }else

  if( c=='s' && strncmp(azArg[0], "separator", n)==0 && nArg==2 ){
    sprintf(p->separator, "%.*s", (int)ArraySize(p->separator)-1, azArg[1]);
  }else

  if( c=='s' && strncmp(azArg[0], "show", n)==0){
    int i;
    fprintf(p->out,"%9.9s: %s\n","echo", p->echoOn ? "on" : "off");
    fprintf(p->out,"%9.9s: %s\n","explain", p->explainPrev.valid ? "on" :"off");
    fprintf(p->out,"%9.9s: %s\n","headers", p->showHeader ? "on" : "off");
    fprintf(p->out,"%9.9s: %s\n","mode", modeDescr[p->mode]);
    fprintf(p->out,"%9.9s: %s\n","nullvalue", p->nullvalue);
    fprintf(p->out,"%9.9s: %s\n","output",
                                 strlen(p->outfile) ? p->outfile : "stdout");
    fprintf(p->out,"%9.9s: %s\n","separator", p->separator);
    fprintf(p->out,"%9.9s: ","width");
    for (i=0;i<(int)ArraySize(p->colWidth) && p->colWidth[i] != 0;i++) {
        fprintf(p->out,"%d ",p->colWidth[i]);
    }
    fprintf(p->out,"\n\n");
  }else

  if( c=='t' && n>1 && strncmp(azArg[0], "tables", n)==0 ){
    char **azResult;
    int nRow, rc;
    char *zErrMsg;
    if( nArg==1 ){
      rc = sqlite_get_table(db,
        "SELECT name FROM sqlite_master "
        "WHERE type IN ('table','view') "
        "ORDER BY name",
        &azResult, &nRow, 0, &zErrMsg
      );
    }else{
      rc = sqlite_get_table_printf(db,
        "SELECT name FROM sqlite_master "
        "WHERE type IN ('table','view') AND name LIKE '%%%q%%' "
        "ORDER BY name",
        &azResult, &nRow, 0, &zErrMsg, azArg[1]
      );
    }
    if( zErrMsg ){
      fprintf(stderr,"Error: %s\n", zErrMsg);
      free(zErrMsg);
    }
    if( rc==SQLITE_OK ){
      int len, maxlen = 0;
      int i, j;
      int nPrintCol, nPrintRow;
      for(i=1; i<=nRow; i++){
        if( azResult[i]==0 ) continue;
        len = strlen(azResult[i]);
        if( len>maxlen ) maxlen = len;
      }
      nPrintCol = 80/(maxlen+2);
      if( nPrintCol<1 ) nPrintCol = 1;
      nPrintRow = (nRow + nPrintCol - 1)/nPrintCol;
      for(i=0; i<nPrintRow; i++){
        for(j=i+1; j<=nRow; j+=nPrintRow){
          char *zSp = j<=nPrintRow ? "" : "  ";
          printf("%s%-*s", zSp, maxlen, azResult[j] ? azResult[j] : "");
        }
        printf("\n");
      }
    }
    sqlite_free_table(azResult);
  }else

  if( c=='t' && n>1 && strncmp(azArg[0], "timeout", n)==0 && nArg>=2 ){
    sqlite_busy_timeout(db, atoi(azArg[1]));
  }else

  if( c=='w' && strncmp(azArg[0], "width", n)==0 ){
    int j;
    for(j=1; j<nArg && j<ArraySize(p->colWidth); j++){
      p->colWidth[j-1] = atoi(azArg[j]);
    }
  }else

  {
    fprintf(stderr, "unknown command or invalid arguments: "
      " \"%s\". Enter \".help\" for help\n", azArg[0]);
  }

  return rc;
}

// 목적: 입력 스트림(in)에서 SQL 명령어나 메타 명령어를 읽어 처리합니다. 
// 입력이 인터랙티브한 경우와 파일로부터 오는 경우를 구분하여 처리합니다.
// 매개변수:
// struct callback_data *p: 콜백 데이터 구조체 포인터.
// FILE *in: 입력을 받을 파일 스트림. NULL이면 인터랙티브 모드입니다.
static void process_input(struct callback_data *p, FILE *in){
  char *zLine;
  char *zSql = 0;
  int nSql = 0;
  char *zErrMsg;
  // 출력 버퍼를 플러시합니다.
  // 한 줄의 입력을 읽습니다.
  while( fflush(p->out), (zLine = one_input_line(zSql, in))!=0 ){
    // 인터럽트가 발생했는지 확인하고, 파일 입력 중이면 루프를 종료합니다. 
    // 인터랙티브 모드에서는 플래그를 초기화합니다.
    if( seenInterrupt ){
      if( in!=0 ) break;
      seenInterrupt = 0;
    }
    // echoOn이 설정되어 있으면 입력된 줄을 출력합니다.
    if( p->echoOn ) printf("%s\n", zLine);
    // 입력이 메타 명령어(.으로 시작)이고 현재 SQL 명령어가 없으면 do_meta_command를 호출하여 처리합니다.
    if( zLine && zLine[0]=='.' && nSql==0 ){
      int rc = do_meta_command(zLine, db, p);
      free(zLine);
      if( rc ) break;
      continue;
    }
    // 메타 명령어가 아니면 SQL 명령어로 간주하여 zSql에 추가합니다.
    if( zSql==0 ){ // 현재 SQL 명령어가 없으면 새로 할당하고 복사합니다.
      int i;
      for(i=0; zLine[i] && isspace(zLine[i]); i++){}
      if( zLine[i]!=0 ){
        nSql = strlen(zLine);
        zSql = malloc( nSql+1 );
        strcpy(zSql, zLine);
      }
    }else{ 
      int len = strlen(zLine);
      zSql = realloc( zSql, nSql + len + 2 );
      if( zSql==0 ){
        fprintf(stderr,"%s: out of memory!\n", Argv0);
        exit(1);
      }
      strcpy(&zSql[nSql++], "\n");
      strcpy(&zSql[nSql], zLine);
      nSql += len;
    } 
    free(zLine);
    // 이미 SQL 명령어가 있는 경우, 새로운 줄을 추가합니다.
    if( zSql && sqlite_complete(zSql) ){
      p->cnt = 0;
      if( sqlite_exec(db, zSql, callback, p, &zErrMsg)!=0
           && zErrMsg!=0 ){
        if( in!=0 && !p->echoOn ) printf("%s\n",zSql);
        printf("SQL error: %s\n", zErrMsg);
        free(zErrMsg);
        zErrMsg = 0;
      }
      free(zSql);
      zSql = 0;
      nSql = 0;
    }
  }
  // 루프가 종료된 후, 미완성된 SQL 명령어가 있다면 경고 메시지를 출력합니다.
  if( zSql ){
    printf("Incomplete SQL: %s\n", zSql);
    free(zSql);
  }
}

/*
** Return a pathname which is the user's home directory.  A
** 0 return indicates an error of some kind.  Space to hold the
** resulting string is obtained from malloc().  The calling
** function should free the result.
*/
static char *find_home_dir(void){
  char *home_dir = NULL;

#if !defined(_WIN32) && !defined(WIN32)
  struct passwd *pwent;
  uid_t uid = getuid();
  while( (pwent=getpwent()) != NULL) {
    if(pwent->pw_uid == uid) {
      home_dir = pwent->pw_dir;
      break;
    }
  }
#endif

  if (!home_dir) {
    home_dir = getenv("HOME");
    if (!home_dir) {
      home_dir = getenv("HOMEPATH"); /* Windows? */
    }
  }

#if defined(_WIN32) || defined(WIN32)
  if (!home_dir) {
    home_dir = "c:";
  }
#endif

  if( home_dir ){
    char *z = malloc( strlen(home_dir)+1 );
    if( z ) strcpy(z, home_dir);
    home_dir = z;
  }

  return home_dir;
}

// 목적: 초기화 스크립트 파일(.sqliterc)을 읽어 처리합니다. sqliterc_override가 제공되면 해당 파일을, 그렇지 않으면 홈 디렉토리의 .sqliterc 파일을 사용합니다.
// 매개변수:
// struct callback_data *p: 콜백 데이터 구조체 포인터.
// char *sqliterc_override: 대체할 .sqliterc 파일 경로. NULL이면 기본 경로를 사용합니다.
static void process_sqliterc(struct callback_data *p, char *sqliterc_override){
  char *home_dir = NULL;
  char *sqliterc = sqliterc_override;
  FILE *in = NULL;
  // sqliterc_override가 NULL이면, find_home_dir을 호출해 홈 디렉토리를 찾습니다.
  if (sqliterc == NULL) {
    home_dir = find_home_dir();
    // 홈 디렉토리가 없으면 오류 메시지를 출력하고 종료합니다.
    if( home_dir==0 ){
      fprintf(stderr,"%s: cannot locate your home directory!\n", Argv0);
      return;
    }
    // .sqliterc 파일 경로를 생성하고 메모리를 할당합니다.
    sqliterc = malloc(strlen(home_dir) + 15);
    if( sqliterc==0 ){
      fprintf(stderr,"%s: out of memory!\n", Argv0);
      exit(1);
    }
    sprintf(sqliterc,"%s/.sqliterc",home_dir);
    free(home_dir);
  }
  // .sqliterc 파일을 열고, 성공하면 process_input을 호출해 파일의 내용을 처리합니다.
  in = fopen(sqliterc,"r");
  if(in) {
    printf("Loading resources from %s\n",sqliterc);
    process_input(p,in);
    fclose(in);
  }
  return;
}

// 목적: callback_data 구조체를 초기화하여 기본 상태를 설정합니다.
void main_init(struct callback_data *data) {
  // memset을 사용해 모든 필드를 0으로 초기화합니다.
  memset(data, 0, sizeof(*data));
  // 기본 출력 모드를 MODE_List로 설정합니다.
  data->mode = MODE_List;
  // 구분자를 |로 설정합니다.
  strcpy(data->separator,"|");
  // 헤더 표시를 꺼둡니다.
  data->showHeader = 0;
  // 기본 프롬프트를 "sqlite> "로, 연속 프롬프트를 " ...> "로 설정합니다.
  strcpy(mainPrompt,"sqlite> ");
  strcpy(continuePrompt,"   ...> ");
}

// 목적: 프로그램의 진입점으로, 명령줄 인수를 처리하고, SQLite 데이터베이스와 상호작용하며, 사용자 입력을 처리합니다.

int main(int argc, char **argv){
  char *zErrMsg = 0; // 에러 메시지를 저장할 포인터.
  struct callback_data data; // 콜백 데이터 구조체.
  int origArgc = argc; // 원래의 인수 개수와 인수 배열을 저장합니다.
  char **origArgv = argv; // 원래의 인수 개수와 인수 배열을 저장합니다.

  Argv0 = argv[0]; // 프로그램 이름 저장.
  main_init(&data); // 콜백 데이터 구조체 초기화.
  process_sqliterc(&data,NULL); // 홈 디렉토리의 .sqliterc 파일을 처리합니다.

#ifdef SIGINT // SIGINT가 정의된 경우, interrupt_handler를 시그널 핸들러로 설정합니다.
  signal(SIGINT, interrupt_handler);
#endif
  while( argc>=2 && argv[1][0]=='-' ){ // 옵션(-)을 처리합니다.
    // -init FILENAME: 초기화 스크립트 파일을 처리합니다.
    // -html, -list, -line, -column: 출력 모드를 설정합니다.
    // -separator STRING: separator를 설정합니다.
    // -nullvalue STRING: nullvalue를 설정합니다.
    // -header, -noheader: 헤더 표시를 켜거나 끕니다.
    // -echo: 에코를 켭니다.
    if( argc>=3 && strcmp(argv[1],"-init")==0 ){
      /* If we get a -init to do, we have to pretend that
      ** it replaced the .sqliterc file. Soooo, in order to
      ** do that we need to start from scratch...*/
      main_init(&data);

      /* treat this file as the sqliterc... */
      process_sqliterc(&data,argv[2]);

      /* fix up the command line so we do not re-read
      ** the option next time around... */
      {
        int i = 1;
        for(i=1;i<=argc-2;i++) {
          argv[i] = argv[i+2];
        }
      }
      origArgc-=2;

      /* and reset the command line options to be re-read.*/
      argv = origArgv;
      argc = origArgc;

    }else if( strcmp(argv[1],"-html")==0 ){
      data.mode = MODE_Html;
      argc--;
      argv++;
    }else if( strcmp(argv[1],"-list")==0 ){
      data.mode = MODE_List;
      argc--;
      argv++;
    }else if( strcmp(argv[1],"-line")==0 ){
      data.mode = MODE_Line;
      argc--;
      argv++;
    }else if( strcmp(argv[1],"-column")==0 ){
      data.mode = MODE_Column;
      argc--;
      argv++;
    }else if( argc>=3 && strcmp(argv[1],"-separator")==0 ){
      sprintf(data.separator,"%.*s",(int)sizeof(data.separator)-1,argv[2]);
      argc -= 2;
      argv += 2;
    }else if( argc>=3 && strcmp(argv[1],"-nullvalue")==0 ){
      sprintf(data.nullvalue,"%.*s",(int)sizeof(data.nullvalue)-1,argv[2]);
      argc -= 2;
      argv += 2;
    }else if( strcmp(argv[1],"-header")==0 ){
      data.showHeader = 1;
      argc--;
      argv++;
    }else if( strcmp(argv[1],"-noheader")==0 ){
      data.showHeader = 0;
      argc--;
      argv++;
    }else if( strcmp(argv[1],"-echo")==0 ){
      data.echoOn = 1;
      argc--;
      argv++;
    }else{
      fprintf(stderr,"%s: unknown option: %s\n", Argv0, argv[1]);
      return 1;
    }
  }
  // 인수 개수가 2 또는 3이 아니면 사용법을 출력하고 종료합니다.
  if( argc!=2 && argc!=3 ){
    fprintf(stderr,"Usage: %s ?OPTIONS? FILENAME ?SQL?\n", Argv0);
    exit(1);
  }
  // sqlite_open을 사용해 데이터베이스를 엽니다. 쓰기 가능 모드(0666)로 시도하고, 
  // 실패 시 읽기 전용 모드(0444)로 재시도합니다.
  data.db = db = sqlite_open(argv[1], 0666, &zErrMsg);
  if( db==0 ){
    data.db = db = sqlite_open(argv[1], 0444, &zErrMsg);
    // 데이터베이스가 열리지 않으면 에러 메시지를 출력하고 종료합니다.
    if( db==0 ){
      if( zErrMsg ){
        fprintf(stderr,"Unable to open database \"%s\": %s\n", argv[1],zErrMsg);
      }else{
        fprintf(stderr,"Unable to open database %s\n", argv[1]);
      }
      exit(1);
    }else{
      fprintf(stderr,"Database \"%s\" opened READ ONLY!\n", argv[1]);
    }
  }
  data.out = stdout;
  // 인수가 3개인 경우 (argc==3):
  if( argc==3 ){
    // 두 번째 인수가 메타 명령어(.으로 시작)인 경우 do_meta_command를 호출하고 종료합니다.
    if( argv[2][0]=='.' ){
      do_meta_command(argv[2], db, &data);
      exit(0);
    }else{ // 그렇지 않으면 SQL 명령어로 간주하여 실행합니다.
      int rc;
      rc = sqlite_exec(db, argv[2], callback, &data, &zErrMsg);
      if( rc!=0 && zErrMsg!=0 ){
        fprintf(stderr,"SQL error: %s\n", zErrMsg);
        exit(1);
      }
    }
  }else{ // 인수가 2개인 경우 (argc==2):
    extern int isatty();
    if( isatty(0) ){ // 입력이 터미널인지(isatty(0)) 확인합니다.
      char *zHome;
      char *zHistory = 0;
      printf(
        "SQLite version %s\n"
        "Enter \".help\" for instructions\n",
        sqlite_version
      );
      zHome = find_home_dir(); // 홈 디렉토리의 히스토리 파일을 읽고, 인터랙티브 모드로 입력을 처리합니다.
      if( zHome && (zHistory = malloc(strlen(zHome)+20))!=0 ){
        sprintf(zHistory,"%s/.sqlite_history", zHome);
      }
      if( zHistory ) read_history(zHistory);
      // 종료 시 히스토리를 저장합니다.
      process_input(&data, 0);
      if( zHistory ){
        stifle_history(100);
        write_history(zHistory);
      }
    }else{ // 터미널이 아닌 경우: 파일 스트림으로부터 입력을 읽어 처리합니다.
      process_input(&data, stdin);
    }
  }
  // 테이블 이름을 초기화하고, 데이터베이스를 닫습니다.
  set_table_name(&data, 0);
  sqlite_close(db);
  // 프로그램을 정상 종료합니다.
  return 0;
}

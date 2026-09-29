# Classify the output of repeated c1_relaxed_publication runs.
#
# Input: runs separated by "--- run N" lines, each followed by the
# binary's combined stdout and stderr and then an "exit N" line, as
# written by the C1 loop in the README's reproduce section.
#
# c1_relaxed_publication builds every field of record N as N*10 plus a
# per-field constant (sequence is N itself), so each observed field
# names the generation it came from. symbol_id is the constant 1 in
# every record and is not used. Each run is classified from the
# generations of the fields it printed, where N is the expected sequence:
#   whole_stale      every printed field from N - 2
#   fresh_seq_mixed  sequence from N, some payload fields from N - 2
#   stale_seq_mixed  sequence from N - 2, some payload fields from N
#   no_tear          exit 0 and no corruption reported
#   other            anything else, including a field from a third
#                    generation and a value that decodes to no
#                    generation, which is printed as X
# The 2 is kCapacity in src/c1_relaxed_publication.cpp, hardcoded here.
#
# Usage: awk -f tools/classify_c1_runs.awk RUNS.txt
# Prints one line per run; the last field is the class.

# bad is tested before the shape of the run, so a single field from a
# third generation makes the whole run other.
function flush(){ if(r=="")return; cls="other"; if(x==0 && t==0)cls="no_tear"; else if(bad)cls="other"; else if(ns==0)cls="other"; else if(nold==ns)cls="whole_stale"; else if(seq==e && nold>0 && nnew+nold==ns)cls="fresh_seq_mixed"; else if(seq==e-2 && nnew>0 && nnew+nold==ns)cls="stale_seq_mixed"; print "run " r " exit " x " expected " e " gens" g " class " cls; r=""}
# The per-field constants of make_record in c1_relaxed_publication.cpp;
# change them only together with it.
BEGIN{FS=","; c["replay_intended_send_ns"]=100; c["capture_wall_time_ns"]=200; c["event_time_ms"]=300; c["transaction_time_ms"]=400; c["bid_price"]=1; c["ask_price"]=2; c["bid_qty"]=3; c["ask_qty"]=4}
/^--- run /{flush(); split($0,a," "); r=a[3]; g=""; e=""; seq=""; ns=0; nold=0; nnew=0; bad=0; t=0; x=""}
# "C1 " with no colon: a clean run prints "C1: no observable corruption
# detected", which must not count as a tear. The colon after
# expected_sequence keeps expected_sequence_minus_capacity from
# overwriting e.
/^C1 observable corruption detected/{t=1}
/^expected_sequence:/{split($0,a," "); e=a[2]}
/^exit /{split($0,a," "); x=a[2]}
# Field rows are name,expected,observed and are matched by name, so the
# field,expected,observed header row and symbol_id match neither rule.
NF==3 && $1=="sequence"{seq=$3; ns++; if($3==e-2)nold++; else if($3==e)nnew++; else bad=1; g=g" "$3}
# A value whose distance from its constant is not a multiple of 10
# decodes to no generation and is printed as X.
NF==3 && ($1 in c){v=$3-c[$1]; if(v%10){bad=1; g=g" X"} else {q=v/10; ns++; if(q==e-2)nold++; else if(q==e)nnew++; else bad=1; g=g" " q}}
END{flush()}

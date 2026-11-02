#!/usr/bin/perl
# SRB2Kart's alignments given in bytes, or as a type's size in bits, where Z_MallocAlign takes a
# number of bits: it reserves (1<<alignbits)-1 extra bytes. SRB2Kart 1.6 asks for 32 and 64 (bytes)
# and for sizeof(T) * 8 (a type's bits: 1<<32, 1<<480). Shifts that large are undefined: a 64-bit
# ARM's wraps (no alignment, and it works), a 32-bit ARM's gives 0, so 4 GB less a byte and a crash
# on the GameShell, at start (the HUD's draw list, HU_Init) and in the title's attract demo (the
# interpolators of polyobjects, objects and scrolling walls, r_fps.c; 2026-11-02, gdb there).
# Each Z_*Align call's last argument is read whole (calls over several lines, nested parentheses):
#   a number of 32, 64, 128 or 256  its number of bits (5, 6, 7, 8)
#   sizeof(...) * 8                 3 (8 bytes: a pointer's or a double's own alignment)
#   a number under 32               left (bits already: r_draw.c's 16)
#   anything else                   fails, to be looked at
# z_zone.c, where the functions are and pass alignbits on, is left alone. The files are rewritten
# in place; prints how many arguments it changed; fails when one is left to look at.
#   srb2kart-align.pl FILE...      (scripts/packages/build-srb2kart.sh)
use strict;
use warnings;

my %bits = (32 => 5, 64 => 6, 128 => 7, 256 => 8);
my ($fixed, $bad) = (0, 0);
for my $f (@ARGV) {
    next if $f =~ m{(^|/)z_zone\.c$};
    open(my $in, '<', $f) or die "$f: $!\n";
    my $s = do { local $/; <$in> };
    close($in);
    my ($out, $pos, $n) = ('', 0, 0);
    while ($s =~ /\bZ_(?:Malloc|Calloc|Realloc)Align\s*\(/g) {
        my $open = pos($s) - 1;
        my ($depth, $i, $last) = (0, $open, $open + 1);
        for (; $i < length($s); $i++) {   # (the matching parenthesis; the last top-level comma)
            my $c = substr($s, $i, 1);
            if ($c eq '(') { $depth++ }
            elsif ($c eq ')') { last if --$depth == 0 }
            elsif ($c eq ',' && $depth == 1) { $last = $i + 1 }
        }
        my $arg = substr($s, $last, $i - $last);
        (my $a = $arg) =~ s/^\s+|\s+$//g;
        my $new;
        if ($a =~ /^\d+$/) {
            if ($a >= 32) { $new = $bits{$a}; unless (defined $new) { print STDERR "$f: an alignment of $a\n"; $bad++; } }
        } elsif ($a =~ /^sizeof\s*\(.*\)\s*\*\s*8$/s) {
            $new = 3;
        } else {
            print STDERR "$f: an alignment to look at: $a\n"; $bad++;
        }
        if (defined $new) {
            (my $lead = $arg) =~ s/\S.*$//s;
            (my $trail = $arg) =~ s/^.*\S//s;
            $out .= substr($s, $pos, $last - $pos) . $lead . $new . $trail;
            $pos = $i;
            $n++;
        }
        pos($s) = $i;
    }
    next unless $n;
    $out .= substr($s, $pos);
    $fixed += $n;
    open(my $o, '>', $f) or die "$f: $!\n";
    print $o $out;
    close($o);
}
print "$fixed\n";
exit($bad ? 1 : 0);

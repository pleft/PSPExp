#!/usr/bin/perl
# SRB2Kart's alignments given in bytes where Z_MallocAlign takes bits: it reserves (1<<alignbits)-1
# extra bytes, and SRB2Kart 1.6 asks for 32 and 64 (bytes). 1<<64 is undefined: a 64-bit ARM's shift
# wraps (no alignment, and it works), a 32-bit ARM's gives 0, so 4 GB less a byte and a crash on the
# GameShell, at start (the HUD's draw list, HU_Init) and in the title's attract demo (the
# interpolation of polyobjects and objects, r_fps.c; 2026-11-02, gdb there). Each Z_*Align call's
# last argument of 32, 64, 128 or 256 becomes its number of bits, calls over several lines included;
# the files are rewritten in place. Prints how many it changed; fails when an alignment of 32 or
# more is still there.
#   srb2kart-align.pl FILE...      (scripts/packages/build-srb2kart.sh)
use strict;
use warnings;

my %bits = (32 => 5, 64 => 6, 128 => 7, 256 => 8);
my $call = qr/Z_(?:Malloc|Calloc|Realloc)Align\s*\((?:[^;()]|\([^()]*\))*?,\s*/;
my ($fixed, $left) = (0, 0);
for my $f (@ARGV) {
    open(my $in, '<', $f) or die "$f: $!\n";
    my $s = do { local $/; <$in> };
    close($in);
    my $n = ($s =~ s/($call)(32|64|128|256)(\s*\))/$1$bits{$2}$3/g) || 0;
    while ($s =~ /$call(\d+)\s*\)/g) { if ($1 >= 32) { print STDERR "$f: an alignment of $1 bits left\n"; $left++; } }
    next unless $n;
    $fixed += $n;
    open(my $out, '>', $f) or die "$f: $!\n";
    print $out $s;
    close($out);
}
print "$fixed\n";
exit($left ? 1 : 0);

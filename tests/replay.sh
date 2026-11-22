#!/bin/sh
# Stands in for a flashing tool in the tests (tests/progress.sh), as
# tests/fake_dap.cpp stands in for a debug adapter: it writes a tool's
# output (a sample in tests/progress/) as the tool did, a piece at a time,
# its bars redrawn with \r, at a chosen pace, and waits where told to.
#
# A sample is lines, each written with a newline after it, unless it ends
# "\c"; in them \r \n \e \b \t \\ and \xHH are those bytes. And:
#   # a comment          (not written)
#   @pace MS             each piece from here on is followed by a wait of MS
#   @hold NAME           wait until $REPLAY_HOLD/NAME is there (12 s at most)
#   @exit N              end with N (else 0)
#
#   sh tests/replay.sh SAMPLE
exec perl -e '
    $| = 1;
    binmode STDOUT;
    my ($pace, $code, $hold) = ($ENV{REPLAY_PACE} // 0, 0, $ENV{REPLAY_HOLD} // ".");
    open(my $f, "<", $ARGV[0]) or die "replay: $ARGV[0]: $!\n";
    while (my $l = <$f>) {
        chomp $l;
        next if $l =~ /^#/;
        if ($l =~ /^\@pace (\d+)$/) { $pace = $1; next }
        if ($l =~ /^\@exit (\d+)$/) { $code = $1; next }
        if ($l =~ /^\@hold (\S+)$/) {
            for (my $w = 0; !-e "$hold/$1" && $w < 600; ++$w) { select(undef, undef, undef, 0.02) }
            next;
        }
        my $end = ($l =~ s/\\c$//) ? "" : "\n";
        $l =~ s/\\(x([0-9a-fA-F]{2})|.)/defined $2 ? chr(hex $2) : $1 eq "r" ? "\r" : $1 eq "n" ? "\n" : $1 eq "e" ? "\e" : $1 eq "b" ? "\b" : $1 eq "t" ? "\t" : $1/ge;
        print $l, $end;
        select(undef, undef, undef, $pace / 1000) if $pace;
    }
    exit $code;
' "$1"

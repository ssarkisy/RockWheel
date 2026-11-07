#!/usr/bin/perl
# lngsize.pl <file.lng>...: the sizes of language files' sections for
# max_language_size.h. MAX_LANGUAGE_SIZE: the largest core section, which the
# core keeps loaded; the second section, of the plugins of Extras, is left out
# of it, as those load it themselves (apps/language.c lang_extras_load()).
# MAX_EXTRAS_SIZE: what such a plugin needs for English's section and the
# largest other's over it.
use strict;
use warnings;

my $HEADER = 4;
my $SUBHEADER = 6;
my ($core, $extras, $english) = (0, 0, 0);
for my $f (@ARGV) {
    open my $fh, '<:raw', $f or die "lngsize.pl: $f: $!\n";
    read($fh, my $head, $HEADER + 2 * $SUBHEADER) == $HEADER + 2 * $SUBHEADER
        or die "lngsize.pl: $f is too short\n";
    close $fh;
    # each subheader: the count of strings, their size and their offset
    my (undef, $c) = unpack('n n', substr($head, $HEADER, 4));
    my (undef, $x) = unpack('n n', substr($head, $HEADER + $SUBHEADER, 4));
    $core = $c if $c > $core;
    if ($f =~ m{(^|/)english\.lng$}) {
        $english = $x;
    } elsif ($x > $extras) {
        $extras = $x;
    }
}
print "#define MAX_LANGUAGE_SIZE $core\n";
print "#define MAX_EXTRAS_SIZE " . ($english + $extras) . "\n";

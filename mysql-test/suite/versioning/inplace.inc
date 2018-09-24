--disable_query_log
if ($MTR_COMBINATION_INPLACE)
{
    let $ALTER_TYPE= algorithm=inplace;
}
if ($MTR_COMBINATION_INSTANT)
{
    let $ALTER_TYPE= lock=none;
}
--enable_query_log

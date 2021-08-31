
void dullahan_impl::platormInitWidevine(std::string cachePath)
{
}

void dullahan_impl::platformAddCommandLines(CefRefPtr<CefCommandLine> command_line)
{
    if (mForceWaveAudio == true)
    {
        // Grouping these together since they're interconnected.
        // The pair, force use of WAV based audio and the second stops
        // CEF using out of process audio which breaks ::waveOutSetVolume()
        // that ise used to control the volume of media in a web page
        command_line->AppendSwitch("force-wave-audio");

        // <ND> This breaks twitch and friends. Allow to not add this via env override (for debugging)
        char* envvar;
        size_t env_len;
        errno_t err = _dupenv_s(&envvar, &env_len, "dlhAudioServiceOutOfProcess");
        if (err) 
            return;

        bool bDisableAudioServiceOutOfProcess { true };
        if(envvar && envvar[0] == '1' )
        {
            bDisableAudioServiceOutOfProcess = false;
        }

        if (bDisableAudioServiceOutOfProcess)
        {
            command_line->AppendSwitchWithValue("disable-features", "AudioServiceOutOfProcess");
        }
    }

}

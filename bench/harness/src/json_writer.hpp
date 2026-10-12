// Billet benchmark harness: a minimal JSON writer (objects, arrays, strings, numbers, booleans).

#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace billet
{

class JsonWriter
{
public:
  void beginObject() { open('{'); }

  void endObject() { close('}'); }

  void beginArray() { open('['); }

  void endArray() { close(']'); }

  // Sets the key for the next value inside an object.
  JsonWriter& key(const std::string& theKey)
  {
    separate();
    writeString(theKey);
    myText += ": ";
    myPendingKey = true;
    return *this;
  }

  void value(const std::string& theValue)
  {
    separate();
    writeString(theValue);
  }

  void value(const char* theValue) { value(std::string(theValue)); }

  void value(bool theValue)
  {
    separate();
    myText += theValue ? "true" : "false";
  }

  void value(int theValue) { number(std::to_string(theValue)); }

  void value(long long theValue) { number(std::to_string(theValue)); }

  void value(size_t theValue) { number(std::to_string(theValue)); }

  void value(double theValue)
  {
    if (!std::isfinite(theValue))
    {
      number("null");
      return;
    }
    char aBuffer[32];
    std::snprintf(aBuffer, sizeof(aBuffer), "%.17g", theValue);
    number(aBuffer);
  }

  void value(const std::vector<double>& theValues)
  {
    beginArray();
    for (double aValue : theValues)
    {
      value(aValue);
    }
    endArray();
  }

  const std::string& text() const { return myText; }

private:
  void open(char theBracket)
  {
    separate();
    myText += theBracket;
    myFirst.push_back(true);
  }

  void close(char theBracket)
  {
    myFirst.pop_back();
    myText += theBracket;
  }

  void number(const std::string& theText)
  {
    separate();
    myText += theText;
  }

  // Writes a comma before every element except the first one of a container.
  void separate()
  {
    if (myPendingKey)
    {
      myPendingKey = false;
      return;
    }
    if (!myFirst.empty())
    {
      if (!myFirst.back())
      {
        myText += ", ";
      }
      myFirst.back() = false;
    }
  }

  void writeString(const std::string& theValue)
  {
    myText += '"';
    for (unsigned char aChar : theValue)
    {
      switch (aChar)
      {
        case '"':
          myText += "\\\"";
          break;
        case '\\':
          myText += "\\\\";
          break;
        case '\n':
          myText += "\\n";
          break;
        case '\r':
          myText += "\\r";
          break;
        case '\t':
          myText += "\\t";
          break;
        default:
          if (aChar < 0x20)
          {
            char aBuffer[8];
            std::snprintf(aBuffer, sizeof(aBuffer), "\\u%04x", aChar);
            myText += aBuffer;
          }
          else
          {
            myText += static_cast<char>(aChar);
          }
      }
    }
    myText += '"';
  }

  std::string       myText;
  std::vector<bool> myFirst;
  bool              myPendingKey = false;
};

} // namespace billet
